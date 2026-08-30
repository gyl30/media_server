package main

import (
	"context"
	"io"
	"log/slog"
	"sync"
	"testing"
	"time"

	"github.com/emiago/sipgo"
	"github.com/emiago/sipgo/sip"
)

func TestRegistrationRefreshStartsBeforeFleetInitialRegistrationCompletes(t *testing.T) {
	identities, err := newIdentitySet("34020000001320000001", "34020000001320000002", 2)
	if err != nil {
		t.Fatal(err)
	}
	deviceOneStarted := make(chan struct{})
	releaseDeviceOne := make(chan struct{})
	deviceZeroRefreshed := make(chan struct{})
	var deviceOneOnce sync.Once
	var deviceZeroCount int
	var deviceOneCount int
	var deviceZeroRegisteredAt time.Time
	var deviceZeroRefreshedAt time.Time
	var mutex sync.Mutex

	platformUA, err := sipgo.NewUA()
	if err != nil {
		t.Fatal(err)
	}
	platform, err := sipgo.NewServer(platformUA)
	if err != nil {
		_ = platformUA.Close()
		t.Fatal(err)
	}
	platform.OnRegister(func(request *sip.Request, transaction sip.ServerTransaction) {
		if request.GetHeader("Authorization") == nil {
			response := sip.NewResponseFromRequest(request, sip.StatusUnauthorized, "Unauthorized", nil)
			response.AppendHeader(sip.NewHeader("WWW-Authenticate", `Digest realm="3402000000", nonce="refresh-test", algorithm=MD5, qop="auth"`))
			_ = transaction.Respond(response)
			return
		}
		deviceID := request.From().Address.User
		mutex.Lock()
		if deviceID == identities.deviceID(1) {
			deviceOneCount++
			deviceOneOnce.Do(func() { close(deviceOneStarted) })
		} else if deviceID == identities.deviceID(0) {
			deviceZeroCount++
			switch deviceZeroCount {
			case 1:
				deviceZeroRegisteredAt = time.Now()
			case 2:
				deviceZeroRefreshedAt = time.Now()
				close(deviceZeroRefreshed)
			}
		}
		mutex.Unlock()
		if deviceID == identities.deviceID(1) {
			<-releaseDeviceOne
		}
		_ = transaction.Respond(sip.NewResponseFromRequest(request, sip.StatusOK, "OK", nil))
	})
	ready := make(chan string, 1)
	platformContext, cancelPlatform := context.WithCancel(t.Context())
	platformContext = context.WithValue(platformContext, sipgo.ListenReadyCtxKey, sipgo.ListenReadyFuncCtxValue(func(_ string, address string) {
		ready <- address
	}))
	platformDone := make(chan error, 1)
	go func() { platformDone <- platform.ListenAndServe(platformContext, "udp", "127.0.0.1:0") }()
	platformAddress := <-ready
	t.Cleanup(func() {
		cancelPlatform()
		_ = platformUA.Close()
		<-platformDone
	})

	fleet := &simulatedFleet{
		cfg: config{
			platformSIP:    platformAddress,
			listen:         "127.0.0.1:0",
			platformID:     "34020000002000000001",
			domain:         "3402000000",
			password:       "12345678",
			registerExpiry: 2 * time.Second,
			registerRate:   1,
			devices:        2,
			sipEndpoints:   1,
			controlWorkers: 1,
		},
		logger:        slog.New(slog.NewTextHandler(io.Discard, nil)),
		identities:    identities,
		endpoints:     make([]fleetEndpoint, 1),
		states:        make([]fleetDeviceState, 2),
		dialogs:       make(map[string]int),
		catalogJobs:   make(chan catalogWork, 1),
		heartbeats:    make(chan int, 1),
		registrations: make(chan int, 1),
		errors:        make(chan error, 1),
	}
	if err := fleet.start(t.Context()); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		select {
		case <-releaseDeviceOne:
		default:
			close(releaseDeviceOne)
		}
		fleet.cancel()
		for index := range fleet.endpoints {
			_ = fleet.endpoints[index].ua.Close()
		}
		fleet.done.Wait()
		<-fleet.endpoints[0].serveDone
	})

	registerDone := make(chan error, 1)
	go func() { registerDone <- fleet.registerAll(t.Context()) }()
	select {
	case <-deviceOneStarted:
	case <-time.After(3 * time.Second):
		t.Fatal("second device did not begin its initial REGISTER")
	}
	select {
	case <-deviceZeroRefreshed:
	case <-time.After(3 * time.Second):
		t.Fatal("first device did not refresh while the fleet was still registering")
	}
	mutex.Lock()
	registrationInterval := deviceZeroRefreshedAt.Sub(deviceZeroRegisteredAt)
	initialDeviceOneRequests := deviceOneCount
	mutex.Unlock()
	if registrationInterval < time.Second {
		t.Fatalf("first refresh arrived after %v, want at least 1s", registrationInterval)
	}
	if initialDeviceOneRequests != 1 {
		t.Fatalf("device with an in-flight initial REGISTER received %d authenticated requests", initialDeviceOneRequests)
	}
	select {
	case err := <-registerDone:
		t.Fatalf("fleet initial registration completed early: %v", err)
	default:
	}
	close(releaseDeviceOne)
	if err := <-registerDone; err != nil {
		t.Fatalf("registerAll() error = %v", err)
	}
}

func TestRegistrationRefreshScheduleSkipsUnregisteredAndPendingDevices(t *testing.T) {
	now := time.Now()
	fleet := &simulatedFleet{
		cfg:    config{registerExpiry: 120 * time.Second},
		states: make([]fleetDeviceState, 3),
	}
	fleet.states[0].registered = true
	fleet.states[0].refreshAt = now
	fleet.states[1].refreshAt = now.Add(-time.Minute)
	fleet.states[2].registered = true
	fleet.states[2].refreshPending = true
	fleet.states[2].refreshAt = now.Add(-time.Minute)

	due := fleet.registrationRefreshesDue(nil, now)
	if len(due) != 1 || due[0] != 0 {
		t.Fatalf("due registrations = %v, want [0]", due)
	}
	if repeated := fleet.registrationRefreshesDue(nil, now.Add(time.Second)); len(repeated) != 0 {
		t.Fatalf("pending registrations were scheduled again: %v", repeated)
	}
}

func TestRegistrationRefreshFailureRemainsDueForRetry(t *testing.T) {
	now := time.Now()
	fleet := &simulatedFleet{
		cfg:    config{registerExpiry: 120 * time.Second},
		states: make([]fleetDeviceState, 1),
	}
	fleet.states[0].registered = true
	fleet.states[0].refreshAt = now

	due := fleet.registrationRefreshesDue(nil, now)
	if len(due) != 1 || due[0] != 0 {
		t.Fatalf("due registrations = %v, want [0]", due)
	}
	if !fleet.states[0].refreshAt.Equal(now) {
		t.Fatalf("refresh deadline moved while request was only queued: got %v want %v", fleet.states[0].refreshAt, now)
	}

	fleet.states[0].refreshPending = false
	retry := fleet.registrationRefreshesDue(nil, now.Add(time.Second))
	if len(retry) != 1 || retry[0] != 0 {
		t.Fatalf("retry registrations = %v, want [0]", retry)
	}
}

func TestRegistrationRefreshSchedulerStopsWithContext(t *testing.T) {
	fleet := &simulatedFleet{
		cfg:           config{registerExpiry: 2 * time.Second},
		states:        make([]fleetDeviceState, 1),
		registrations: make(chan int, 1),
	}
	fleet.states[0].registered = true
	fleet.states[0].refreshAt = time.Now()
	ctx, cancel := context.WithCancel(t.Context())
	fleet.done.Add(1)
	go fleet.runRegistrationScheduler(ctx)
	cancel()
	done := make(chan struct{})
	go func() {
		fleet.done.Wait()
		close(done)
	}()
	select {
	case <-done:
	case <-time.After(time.Second):
		t.Fatal("registration refresh scheduler did not stop")
	}
	select {
	case index := <-fleet.registrations:
		t.Fatalf("registration %d was queued after shutdown", index)
	default:
	}
}
