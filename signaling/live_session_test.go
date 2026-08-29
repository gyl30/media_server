package main

import (
	"context"
	"errors"
	"io"
	"log/slog"
	"net"
	"net/http"
	"net/http/httptest"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/emiago/sipgo"
	"github.com/emiago/sipgo/sip"
)

func TestLiveSessionInviteAckByeAndMediaLifecycle(t *testing.T) {
	device := startLiveTestDevice(t, func(request *sip.Request) ([]byte, int) {
		return []byte(strings.ReplaceAll(string(request.Body()), "a=recvonly", "a=sendonly")), sip.StatusOK
	})
	platform, _ := startRegistrar(t, testConfig())
	registerLiveTestDevice(t, platform, device.addr)
	mediaRegistry, mediaServer, creates, deletes := startLiveTestMediaServer(t)
	allocator, err := newSSRCAllocator(platform.cfg.sipDomain)
	if err != nil {
		t.Fatalf("newSSRCAllocator() error = %v", err)
	}
	live := newLiveService(platform, mediaRegistry, newMediaServerHTTPClient(time.Second), allocator, slog.New(slog.NewTextHandler(io.Discard, nil)))

	view, err := live.startLive(context.Background(), testDeviceID, testChannelID)
	if err != nil {
		t.Fatalf("startLive() error = %v", err)
	}
	if view.state != liveStreaming || view.streamName != "gb/"+testDeviceID+"/"+testChannelID || view.ssrc != 200000001 || view.rtpPort != 40000 {
		t.Fatalf("live view = %+v", view)
	}
	select {
	case request := <-device.invites:
		if request.Method != sip.INVITE || request.ContentType() == nil || request.ContentType().Value() != "application/sdp" {
			t.Fatalf("INVITE = %s content-type = %v", request.StartLine(), request.ContentType())
		}
		if request.Recipient.User != testChannelID || request.Destination() != device.addr {
			t.Fatalf("INVITE recipient = %s destination = %s", request.Recipient.String(), request.Destination())
		}
	case <-time.After(2 * time.Second):
		t.Fatal("INVITE was not received")
	}
	select {
	case <-device.acks:
	case <-time.After(2 * time.Second):
		t.Fatal("ACK was not received")
	}
	if creates.Load() != 1 || deletes.Load() != 0 {
		t.Fatalf("media calls create=%d delete=%d", creates.Load(), deletes.Load())
	}
	if _, err := live.startLive(context.Background(), testDeviceID, testChannelID); !errors.Is(err, errLiveExists) {
		t.Fatalf("duplicate error = %v", err)
	}

	if err := live.stopLive(context.Background(), testDeviceID, testChannelID); err != nil {
		t.Fatalf("stopLive() error = %v", err)
	}
	select {
	case <-device.byes:
	case <-time.After(2 * time.Second):
		t.Fatal("BYE was not received")
	}
	if deletes.Load() != 1 || live.len() != 0 || allocator.activeCount() != 0 {
		t.Fatalf("cleanup delete=%d live=%d ssrc=%d", deletes.Load(), live.len(), allocator.activeCount())
	}
	_ = mediaServer
}

func TestLiveSessionInviteFailureDeletesMedia(t *testing.T) {
	device := startLiveTestDevice(t, func(_ *sip.Request) ([]byte, int) { return nil, sip.StatusBusyHere })
	platform, _ := startRegistrar(t, testConfig())
	registerLiveTestDevice(t, platform, device.addr)
	mediaRegistry, _, creates, deletes := startLiveTestMediaServer(t)
	allocator, _ := newSSRCAllocator(platform.cfg.sipDomain)
	live := newLiveService(platform, mediaRegistry, newMediaServerHTTPClient(time.Second), allocator, slog.New(slog.NewTextHandler(io.Discard, nil)))

	if _, err := live.startLive(context.Background(), testDeviceID, testChannelID); err == nil {
		t.Fatal("startLive() succeeded")
	}
	if creates.Load() != 1 || deletes.Load() != 1 || live.len() != 0 || allocator.activeCount() != 0 {
		t.Fatalf("cleanup create=%d delete=%d live=%d ssrc=%d", creates.Load(), deletes.Load(), live.len(), allocator.activeCount())
	}
}

func TestLiveSessionInviteTimeoutCancelsAndCleansUp(t *testing.T) {
	device := startLiveTestDevice(t, func(_ *sip.Request) ([]byte, int) { return nil, sip.StatusTrying })
	platform, _ := startRegistrar(t, testConfig())
	registerLiveTestDevice(t, platform, device.addr)
	mediaRegistry, _, creates, deletes := startLiveTestMediaServer(t)
	allocator, _ := newSSRCAllocator(platform.cfg.sipDomain)
	live := newLiveService(platform, mediaRegistry, newMediaServerHTTPClient(time.Second), allocator, slog.New(slog.NewTextHandler(io.Discard, nil)))
	live.inviteTimeout = 50 * time.Millisecond

	if _, err := live.startLive(context.Background(), testDeviceID, testChannelID); !errors.Is(err, context.DeadlineExceeded) {
		t.Fatalf("startLive() error = %v", err)
	}
	select {
	case <-device.cancels:
	case <-time.After(2 * time.Second):
		t.Fatal("CANCEL was not received")
	}
	if creates.Load() != 1 || deletes.Load() != 1 || live.len() != 0 || allocator.activeCount() != 0 {
		t.Fatalf("cleanup create=%d delete=%d live=%d ssrc=%d", creates.Load(), deletes.Load(), live.len(), allocator.activeCount())
	}
	select {
	case <-device.acks:
		t.Fatal("timed out INVITE was acknowledged")
	case <-device.byes:
		t.Fatal("timed out INVITE sent BYE")
	default:
	}
}

func TestLiveSessionStopWhileInvitingCancelsAndCleansUp(t *testing.T) {
	device := startLiveTestDevice(t, func(_ *sip.Request) ([]byte, int) { return nil, sip.StatusTrying })
	platform, _ := startRegistrar(t, testConfig())
	registerLiveTestDevice(t, platform, device.addr)
	mediaRegistry, _, _, deletes := startLiveTestMediaServer(t)
	allocator, _ := newSSRCAllocator(platform.cfg.sipDomain)
	live := newLiveService(platform, mediaRegistry, newMediaServerHTTPClient(time.Second), allocator, slog.New(slog.NewTextHandler(io.Discard, nil)))
	started := make(chan error, 1)
	go func() {
		_, err := live.startLive(context.Background(), testDeviceID, testChannelID)
		started <- err
	}()
	select {
	case <-device.provisionals:
	case <-time.After(2 * time.Second):
		t.Fatal("provisional response was not sent")
	}
	if err := live.stopLive(context.Background(), testDeviceID, testChannelID); err != nil {
		t.Fatalf("stopLive() error = %v", err)
	}
	if err := <-started; !errors.Is(err, context.Canceled) {
		t.Fatalf("startLive() error = %v", err)
	}
	select {
	case <-device.cancels:
	case <-time.After(2 * time.Second):
		t.Fatal("CANCEL was not received")
	}
	if deletes.Load() != 1 || live.len() != 0 || allocator.activeCount() != 0 {
		t.Fatalf("cleanup delete=%d live=%d ssrc=%d", deletes.Load(), live.len(), allocator.activeCount())
	}
}

func TestLiveSessionStopWhilePreparingReleasesSessionAndSSRC(t *testing.T) {
	device := startLiveTestDevice(t, func(request *sip.Request) ([]byte, int) {
		return []byte(strings.ReplaceAll(string(request.Body()), "a=recvonly", "a=sendonly")), sip.StatusOK
	})
	platform, _ := startRegistrar(t, testConfig())
	registerLiveTestDevice(t, platform, device.addr)
	createStarted := make(chan struct{})
	releaseCreate := make(chan struct{})
	defer close(releaseCreate)
	deletes := &atomic.Int32{}
	mediaServer := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
		if request.URL.Path == "/gb28181/create" {
			close(createStarted)
			<-releaseCreate
			return
		}
		if request.URL.Path == "/gb28181/delete" {
			deletes.Add(1)
			_, _ = io.WriteString(writer, `{"result":"ok"}`)
			return
		}
		http.NotFound(writer, request)
	}))
	t.Cleanup(mediaServer.Close)
	mediaRegistry := newMediaServerRegistry()
	if err := mediaRegistry.register(mediaServerRegistration{
		ServerID: "media-1", InstanceID: "instance-a", ControlURL: mediaServer.URL, MediaIP: "127.0.0.1",
	}, time.Now()); err != nil {
		t.Fatalf("media registry register error = %v", err)
	}
	allocator, _ := newSSRCAllocator(platform.cfg.sipDomain)
	live := newLiveService(platform, mediaRegistry, newMediaServerHTTPClient(time.Second), allocator, slog.New(slog.NewTextHandler(io.Discard, nil)))
	started := make(chan error, 1)
	go func() {
		_, err := live.startLive(context.Background(), testDeviceID, testChannelID)
		started <- err
	}()
	select {
	case <-createStarted:
	case <-time.After(2 * time.Second):
		t.Fatal("media create was not received")
	}
	if err := live.stopLive(context.Background(), testDeviceID, testChannelID); err != nil {
		t.Fatalf("stopLive() error = %v", err)
	}
	if err := <-started; !errors.Is(err, context.Canceled) {
		t.Fatalf("startLive() error = %v", err)
	}
	if deletes.Load() != 1 || live.len() != 0 || allocator.activeCount() != 0 {
		t.Fatalf("cleanup delete=%d live=%d ssrc=%d", deletes.Load(), live.len(), allocator.activeCount())
	}
	select {
	case <-device.invites:
		t.Fatal("INVITE was sent before media create completed")
	default:
	}
}

func TestLiveSessionInvalidAnswerDeletesMedia(t *testing.T) {
	device := startLiveTestDevice(t, func(request *sip.Request) ([]byte, int) {
		return []byte(strings.Replace(string(request.Body()), "PS/90000", "H264/90000", 1)), sip.StatusOK
	})
	platform, _ := startRegistrar(t, testConfig())
	registerLiveTestDevice(t, platform, device.addr)
	mediaRegistry, _, _, deletes := startLiveTestMediaServer(t)
	allocator, _ := newSSRCAllocator(platform.cfg.sipDomain)
	live := newLiveService(platform, mediaRegistry, newMediaServerHTTPClient(time.Second), allocator, slog.New(slog.NewTextHandler(io.Discard, nil)))

	if _, err := live.startLive(context.Background(), testDeviceID, testChannelID); err == nil {
		t.Fatal("startLive() succeeded")
	}
	select {
	case <-device.acks:
	case <-time.After(2 * time.Second):
		t.Fatal("invalid SDP response was not acknowledged")
	}
	select {
	case <-device.byes:
	case <-time.After(2 * time.Second):
		t.Fatal("invalid SDP dialog was not terminated")
	}
	if deletes.Load() != 1 || live.len() != 0 || allocator.activeCount() != 0 {
		t.Fatalf("cleanup delete=%d live=%d ssrc=%d", deletes.Load(), live.len(), allocator.activeCount())
	}
}

func TestLiveSessionRejectsDuplicateInEveryActiveState(t *testing.T) {
	platform := newTestSIPServer(t, "127.0.0.1:0")
	registerLiveTestDevice(t, platform, "127.0.0.1:5062")
	mediaRegistry, _, _, _ := startLiveTestMediaServer(t)
	allocator, _ := newSSRCAllocator(platform.cfg.sipDomain)
	live := newLiveService(platform, mediaRegistry, newMediaServerHTTPClient(time.Second), allocator, slog.New(slog.NewTextHandler(io.Discard, nil)))
	key := liveKey{deviceID: testDeviceID, channelID: testChannelID}

	for _, state := range []liveState{livePreparing, liveInviting, liveStreaming, liveStopping} {
		_, cancel := context.WithCancel(context.Background())
		live.sessions[key] = &liveSession{key: key, state: state, cancel: cancel, established: make(chan struct{})}
		if _, err := live.startLive(context.Background(), testDeviceID, testChannelID); !errors.Is(err, errLiveExists) {
			t.Fatalf("state %s duplicate error = %v", state, err)
		}
		cancel()
		delete(live.sessions, key)
	}
	if allocator.activeCount() != 0 {
		t.Fatalf("active SSRCs = %d", allocator.activeCount())
	}
}

func TestLiveSessionRemoteByeDeletesMedia(t *testing.T) {
	device := startLiveTestDevice(t, func(request *sip.Request) ([]byte, int) {
		return []byte(strings.ReplaceAll(string(request.Body()), "a=recvonly", "a=sendonly")), sip.StatusOK
	})
	platform, _ := startRegistrar(t, testConfig())
	registerLiveTestDevice(t, platform, device.addr)
	mediaRegistry, _, _, deletes := startLiveTestMediaServer(t)
	allocator, _ := newSSRCAllocator(platform.cfg.sipDomain)
	live := newLiveService(platform, mediaRegistry, newMediaServerHTTPClient(time.Second), allocator, slog.New(slog.NewTextHandler(io.Discard, nil)))
	if _, err := live.startLive(context.Background(), testDeviceID, testChannelID); err != nil {
		t.Fatalf("startLive() error = %v", err)
	}
	<-device.acks
	dialog := <-device.dialogs
	byeDone := make(chan error, 1)
	go func() {
		ctx, cancel := context.WithTimeout(context.Background(), time.Second)
		defer cancel()
		byeDone <- dialog.Bye(ctx)
	}()
	if err := <-byeDone; err != nil {
		t.Fatalf("device Bye() error = %v", err)
	}
	deadline := time.Now().Add(2 * time.Second)
	for deletes.Load() != 1 || live.len() != 0 {
		if time.Now().After(deadline) {
			t.Fatal("remote BYE cleanup did not finish")
		}
		time.Sleep(time.Millisecond)
	}
}

func TestLiveSessionRejectsByeOutsideDialog(t *testing.T) {
	device := startLiveTestDevice(t, func(request *sip.Request) ([]byte, int) {
		return []byte(strings.ReplaceAll(string(request.Body()), "a=recvonly", "a=sendonly")), sip.StatusOK
	})
	cfg := testConfig()
	platform, addr := startRegistrar(t, cfg)
	registerLiveTestDevice(t, platform, device.addr)
	mediaRegistry, _, _, deletes := startLiveTestMediaServer(t)
	allocator, _ := newSSRCAllocator(platform.cfg.sipDomain)
	live := newLiveService(platform, mediaRegistry, newMediaServerHTTPClient(time.Second), allocator, slog.New(slog.NewTextHandler(io.Discard, nil)))
	if _, err := live.startLive(context.Background(), testDeviceID, testChannelID); err != nil {
		t.Fatalf("startLive() error = %v", err)
	}
	<-device.acks

	live.mu.Lock()
	callID := live.sessions[liveKey{deviceID: testDeviceID, channelID: testChannelID}].callID
	live.mu.Unlock()
	recipient := recipientFor(t, addr)
	recipient.User = cfg.sipID
	request := sip.NewRequest(sip.BYE, recipient)
	fromParams := sip.NewParams()
	fromParams.Add("tag", "wrong-device-tag")
	toParams := sip.NewParams()
	toParams.Add("tag", "wrong-platform-tag")
	request.AppendHeader(&sip.FromHeader{Address: sip.Uri{Scheme: "sip", User: testDeviceID, Host: cfg.sipDomain}, Params: fromParams})
	request.AppendHeader(&sip.ToHeader{Address: sip.Uri{Scheme: "sip", User: cfg.sipID, Host: cfg.sipDomain}, Params: toParams})
	callIDHeader := sip.CallIDHeader(callID)
	request.AppendHeader(&callIDHeader)
	request.AppendHeader(&sip.CSeqHeader{SeqNo: 2, MethodName: sip.BYE})
	request.SetTransport("UDP")
	if response := doSIP(t, device.client, request); response.StatusCode != sip.StatusCallTransactionDoesNotExists {
		t.Fatalf("invalid BYE status = %d", response.StatusCode)
	}
	if deletes.Load() != 0 || live.len() != 1 {
		t.Fatalf("invalid BYE cleanup delete=%d live=%d", deletes.Load(), live.len())
	}
	if err := live.stopLive(context.Background(), testDeviceID, testChannelID); err != nil {
		t.Fatalf("stopLive() error = %v", err)
	}
}

func TestLiveSessionByeTimeoutStillDeletesMedia(t *testing.T) {
	device := startLiveTestDevice(t, func(request *sip.Request) ([]byte, int) {
		return []byte(strings.ReplaceAll(string(request.Body()), "a=recvonly", "a=sendonly")), sip.StatusOK
	})
	platform, _ := startRegistrar(t, testConfig())
	registerLiveTestDevice(t, platform, device.addr)
	mediaRegistry, _, _, deletes := startLiveTestMediaServer(t)
	allocator, _ := newSSRCAllocator(platform.cfg.sipDomain)
	live := newLiveService(platform, mediaRegistry, newMediaServerHTTPClient(time.Second), allocator, slog.New(slog.NewTextHandler(io.Discard, nil)))
	live.byeTimeout = 50 * time.Millisecond
	if _, err := live.startLive(context.Background(), testDeviceID, testChannelID); err != nil {
		t.Fatalf("startLive() error = %v", err)
	}
	<-device.acks
	device.stop()
	if err := live.stopLive(context.Background(), testDeviceID, testChannelID); err == nil {
		t.Fatal("stopLive() succeeded without BYE response")
	}
	if deletes.Load() != 1 || live.len() != 0 || allocator.activeCount() != 0 {
		t.Fatalf("cleanup delete=%d live=%d ssrc=%d", deletes.Load(), live.len(), allocator.activeCount())
	}
}

func TestDeviceAndMediaServerOfflineUseLiveCleanup(t *testing.T) {
	for _, mediaOffline := range []bool{false, true} {
		name := "device"
		if mediaOffline {
			name = "media_server"
		}
		t.Run(name, func(t *testing.T) {
			device := startLiveTestDevice(t, func(request *sip.Request) ([]byte, int) {
				return []byte(strings.ReplaceAll(string(request.Body()), "a=recvonly", "a=sendonly")), sip.StatusOK
			})
			platform, _ := startRegistrar(t, testConfig())
			registerLiveTestDevice(t, platform, device.addr)
			mediaRegistry, _, _, deletes := startLiveTestMediaServer(t)
			allocator, _ := newSSRCAllocator(platform.cfg.sipDomain)
			live := newLiveService(platform, mediaRegistry, newMediaServerHTTPClient(time.Second), allocator, slog.New(slog.NewTextHandler(io.Discard, nil)))
			if _, err := live.startLive(context.Background(), testDeviceID, testChannelID); err != nil {
				t.Fatalf("startLive() error = %v", err)
			}
			<-device.acks
			if mediaOffline {
				instance, _ := mediaRegistry.selectOnline()
				live.mediaServerOffline(context.Background(), instance)
				if deletes.Load() != 0 {
					t.Fatalf("offline media server DELETE calls = %d", deletes.Load())
				}
			} else {
				live.deviceOffline(context.Background(), testDeviceID)
				if deletes.Load() != 1 {
					t.Fatalf("device offline DELETE calls = %d", deletes.Load())
				}
				if _, ok := platform.channels.get(testDeviceID, testChannelID); ok {
					t.Fatal("offline device channel remains")
				}
			}
			if live.len() != 0 || allocator.activeCount() != 0 {
				t.Fatalf("cleanup live=%d ssrc=%d", live.len(), allocator.activeCount())
			}
		})
	}
}

func TestStoppingSessionWaitsForCleanup(t *testing.T) {
	platform := newTestSIPServer(t, "127.0.0.1:0")
	mediaRegistry := newMediaServerRegistry()
	allocator, _ := newSSRCAllocator(platform.cfg.sipDomain)
	live := newLiveService(platform, mediaRegistry, newMediaServerHTTPClient(time.Second), allocator, slog.New(slog.NewTextHandler(io.Discard, nil)))
	key := liveKey{deviceID: testDeviceID, channelID: testChannelID}
	_, cancel := context.WithCancel(context.Background())
	session := &liveSession{key: key, state: liveStopping, cancel: cancel, established: make(chan struct{}), done: make(chan struct{})}
	live.sessions[key] = session
	stopped := make(chan error, 1)
	go func() { stopped <- live.stopLive(context.Background(), testDeviceID, testChannelID) }()
	select {
	case err := <-stopped:
		t.Fatalf("stopLive() returned before cleanup: %v", err)
	case <-time.After(20 * time.Millisecond):
	}
	close(session.done)
	if err := <-stopped; err != nil {
		t.Fatalf("stopLive() error = %v", err)
	}
}

type liveTestDevice struct {
	addr         string
	client       *sipgo.Client
	invites      chan *sip.Request
	messages     chan *sip.Request
	provisionals chan struct{}
	cancels      chan struct{}
	acks         chan struct{}
	byes         chan struct{}
	dialogs      chan *sipgo.DialogServerSession
	stop         func()
}

func startLiveTestDevice(t *testing.T, answer func(*sip.Request) ([]byte, int)) liveTestDevice {
	t.Helper()
	ua, err := sipgo.NewUA(sipgo.WithUserAgent(testDeviceID), sipgo.WithUserAgentHostname("127.0.0.1"))
	if err != nil {
		t.Fatalf("NewUA() error = %v", err)
	}
	server, err := sipgo.NewServer(ua)
	if err != nil {
		_ = ua.Close()
		t.Fatalf("NewServer() error = %v", err)
	}
	client, err := sipgo.NewClient(ua, sipgo.WithClientHostname("127.0.0.1"))
	if err != nil {
		_ = ua.Close()
		t.Fatalf("NewClient() error = %v", err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	ready := make(chan string, 1)
	ctx = context.WithValue(ctx, sipgo.ListenReadyCtxKey, sipgo.ListenReadyFuncCtxValue(func(_ string, addr string) { ready <- addr }))
	var dialogs atomic.Pointer[sipgo.DialogServerCache]
	result := liveTestDevice{
		client:       client,
		invites:      make(chan *sip.Request, 1),
		messages:     make(chan *sip.Request, 2),
		provisionals: make(chan struct{}, 1),
		cancels:      make(chan struct{}, 1),
		acks:         make(chan struct{}, 1),
		byes:         make(chan struct{}, 1),
		dialogs:      make(chan *sipgo.DialogServerSession, 1),
	}
	server.OnMessage(func(request *sip.Request, transaction sip.ServerTransaction) {
		if err := transaction.Respond(sip.NewResponseFromRequest(request, sip.StatusOK, "OK", nil)); err != nil {
			t.Errorf("MESSAGE response error = %v", err)
			return
		}
		result.messages <- request.Clone()
	})
	server.OnInvite(func(request *sip.Request, transaction sip.ServerTransaction) {
		result.invites <- request.Clone()
		dialog, err := dialogs.Load().ReadInvite(request, transaction)
		if err != nil {
			t.Errorf("ReadInvite() error = %v", err)
			return
		}
		if !transaction.OnCancel(func(*sip.Request) {
			select {
			case result.cancels <- struct{}{}:
			default:
			}
		}) {
			t.Error("failed to register CANCEL observer")
			return
		}
		result.dialogs <- dialog
		body, status := answer(request)
		if status == sip.StatusOK {
			if err := dialog.Respond(sip.StatusTrying, "Trying", nil); err != nil {
				t.Errorf("Respond(Trying) error = %v", err)
				return
			}
			if err := dialog.RespondSDP(body); err != nil {
				if err.Error() != "No ACK received" || len(result.byes) == 0 {
					t.Errorf("RespondSDP() error = %v", err)
				}
			}
			return
		}
		if err := dialog.Respond(status, "Rejected", nil); err != nil {
			t.Errorf("Respond(rejected) error = %v", err)
		}
		if status >= 100 && status < 200 {
			result.provisionals <- struct{}{}
			<-dialog.Context().Done()
			select {
			case <-transaction.Acks():
			case <-time.After(time.Second):
			}
		}
	})
	server.OnCancel(func(*sip.Request, sip.ServerTransaction) {
		select {
		case result.cancels <- struct{}{}:
		default:
		}
	})
	server.OnAck(func(request *sip.Request, transaction sip.ServerTransaction) {
		select {
		case result.acks <- struct{}{}:
		default:
		}
		if err := dialogs.Load().ReadAck(request, transaction); err != nil {
			if !errors.Is(err, sipgo.ErrDialogDoesNotExists) {
				t.Errorf("ReadAck() error = %v", err)
			}
		}
	})
	server.OnBye(func(request *sip.Request, transaction sip.ServerTransaction) {
		result.byes <- struct{}{}
		if err := dialogs.Load().ReadBye(request, transaction); err != nil {
			t.Errorf("ReadBye() error = %v", err)
			return
		}
	})
	done := make(chan error, 1)
	go func() { done <- server.ListenAndServe(ctx, "udp", "127.0.0.1:0") }()
	result.addr = waitReady(t, ready)
	host, portText, err := net.SplitHostPort(result.addr)
	if err != nil {
		t.Fatalf("SplitHostPort() error = %v", err)
	}
	port, err := strconv.Atoi(portText)
	if err != nil {
		t.Fatalf("Atoi() error = %v", err)
	}
	dialogs.Store(sipgo.NewDialogServerCache(client, sip.ContactHeader{Address: sip.Uri{Scheme: "sip", User: testDeviceID, Host: host, Port: port}}))
	var stopOnce sync.Once
	result.stop = func() {
		stopOnce.Do(func() {
			cancel()
			select {
			case err := <-done:
				if err != nil && !errors.Is(err, context.Canceled) {
					t.Errorf("device server error = %v", err)
				}
			case <-time.After(2 * time.Second):
				t.Error("device server did not stop")
			}
			_ = ua.Close()
		})
	}
	t.Cleanup(result.stop)
	return result
}

func registerLiveTestDevice(t *testing.T, platform *sipServer, deviceAddr string) {
	t.Helper()
	host, portText, err := net.SplitHostPort(deviceAddr)
	if err != nil {
		t.Fatalf("SplitHostPort() error = %v", err)
	}
	port, err := strconv.Atoi(portText)
	if err != nil {
		t.Fatalf("Atoi() error = %v", err)
	}
	platform.devices.register(registeredDevice{
		id:             testDeviceID,
		contact:        sip.Uri{Scheme: "sip", User: testDeviceID, Host: host, Port: port},
		remoteEndpoint: deviceAddr,
		expiresAt:      time.Now().Add(time.Hour),
		lastHeartbeat:  time.Now(),
		online:         true,
	})
	platform.channels.beginQuery(testDeviceID, 1)
	if err := platform.channels.apply(catalogResponse{
		CmdType: "Catalog", SN: 1, DeviceID: testDeviceID, SumNum: 1,
		DeviceList: struct {
			Num   int              `xml:"Num,attr"`
			Items []catalogChannel `xml:"Item"`
		}{Num: 1, Items: []catalogChannel{{DeviceID: testChannelID, ParentID: testDeviceID, Status: "ON"}}},
	}); err != nil {
		t.Fatalf("channels.apply() error = %v", err)
	}
}

func startLiveTestMediaServer(t *testing.T) (*mediaServerRegistry, *httptest.Server, *atomic.Int32, *atomic.Int32) {
	t.Helper()
	creates := &atomic.Int32{}
	deletes := &atomic.Int32{}
	server := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
		writer.Header().Set("Content-Type", "application/json")
		switch request.URL.Path {
		case "/gb28181/create":
			creates.Add(1)
			writer.WriteHeader(http.StatusCreated)
			_, _ = io.WriteString(writer, `{"result":"ok","rtp_port":40000,"rtcp_port":40001}`)
		case "/gb28181/delete":
			deletes.Add(1)
			_, _ = io.WriteString(writer, `{"result":"ok"}`)
		default:
			http.NotFound(writer, request)
		}
	}))
	t.Cleanup(server.Close)
	registry := newMediaServerRegistry()
	if err := registry.register(mediaServerRegistration{
		ServerID: "media-1", InstanceID: "instance-a", ControlURL: server.URL, MediaIP: "127.0.0.1",
	}, time.Now()); err != nil {
		t.Fatalf("media registry register error = %v", err)
	}
	return registry, server, creates, deletes
}
