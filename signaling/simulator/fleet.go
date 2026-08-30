package main

import (
	"context"
	"encoding/xml"
	"fmt"
	"log/slog"
	"net"
	"runtime"
	"strconv"
	"sync"
	"sync/atomic"
	"time"

	"github.com/emiago/sipgo"
	"github.com/emiago/sipgo/sip"
)

type fleetEndpoint struct {
	ua         *sipgo.UserAgent
	server     *sipgo.Server
	client     *sipgo.Client
	listenAddr string
	serveDone  chan error
}

type fleetDeviceState struct {
	dialog         *sipgo.DialogServerSession
	dialogID       string
	target         mediaTarget
	heartbeatSN    atomic.Uint32
	refreshAt      time.Time
	cataloged      bool
	registered     bool
	refreshPending bool
	streaming      bool
}

type catalogWork struct {
	endpoint int
	device   int
	sn       int
}

type fleetCounters struct {
	registered       atomic.Uint64
	registerRefresh  atomic.Uint64
	registerErrors   atomic.Uint64
	catalogResponses atomic.Uint64
	heartbeats       atomic.Uint64
	heartbeatErrors  atomic.Uint64
	invites          atomic.Uint64
	acks             atomic.Uint64
	byes             atomic.Uint64
	liveStarted      atomic.Uint64
	liveStopped      atomic.Uint64
}

type simulatedFleet struct {
	cfg           config
	logger        *slog.Logger
	identities    identitySet
	control       controlClient
	media         *mediaEngine
	endpoints     []fleetEndpoint
	states        []fleetDeviceState
	dialogs       map[string]int
	mutex         sync.Mutex
	ctx           context.Context
	cancel        context.CancelFunc
	done          sync.WaitGroup
	catalogJobs   chan catalogWork
	heartbeats    chan int
	registrations chan int
	errors        chan error
	counters      fleetCounters
}

func newSimulatedFleet(ctx context.Context, cfg config, source *sharedMediaSource, logger *slog.Logger) (*simulatedFleet, error) {
	identities, err := newIdentitySet(cfg.deviceID, cfg.channelID, cfg.devices)
	if err != nil {
		return nil, err
	}
	media, err := startMediaEngine(ctx, source, cfg.mediaBind, cfg.devices, cfg.mediaWorkers, cfg.phaseBuckets, cfg.batchSize, packetLoss{percent: cfg.packetLossPercent, seed: cfg.seed})
	if err != nil {
		return nil, err
	}
	fleet := &simulatedFleet{
		cfg:           cfg,
		logger:        logger,
		identities:    identities,
		control:       newControlClient(cfg.controlURL),
		media:         media,
		endpoints:     make([]fleetEndpoint, cfg.sipEndpoints),
		states:        make([]fleetDeviceState, cfg.devices),
		dialogs:       make(map[string]int, cfg.liveCount),
		catalogJobs:   make(chan catalogWork, cfg.controlWorkers*4),
		heartbeats:    make(chan int, cfg.controlWorkers*4),
		registrations: make(chan int, cfg.controlWorkers*4),
		errors:        make(chan error, 1),
	}
	for index := range fleet.states {
		fleet.states[index].heartbeatSN.Store(2)
	}
	if err := fleet.start(ctx); err != nil {
		_ = media.stop()
		return nil, err
	}
	return fleet, nil
}

func (f *simulatedFleet) start(ctx context.Context) error {
	runContext, cancel := context.WithCancel(ctx)
	f.ctx = runContext
	f.cancel = cancel
	for index := range f.endpoints {
		if err := f.startEndpoint(runContext, index); err != nil {
			cancel()
			for previous := range index {
				_ = f.endpoints[previous].ua.Close()
			}
			return err
		}
	}
	for range f.cfg.controlWorkers {
		f.done.Add(2)
		go f.runCatalogWorker(runContext)
		go f.runDeviceWorker(runContext)
	}
	f.done.Add(1)
	go f.runRegistrationScheduler(runContext)
	return nil
}

func (f *simulatedFleet) startEndpoint(ctx context.Context, index int) error {
	listen, err := endpointListenAddress(f.cfg.listen, index)
	if err != nil {
		return err
	}
	host, _, err := net.SplitHostPort(listen)
	if err != nil {
		return err
	}
	ua, err := sipgo.NewUA(
		sipgo.WithUserAgent("gb28181-simulator"),
		sipgo.WithUserAgentHostname(host),
	)
	if err != nil {
		return err
	}
	server, err := sipgo.NewServer(ua, sipgo.WithServerLogger(f.logger))
	if err != nil {
		_ = ua.Close()
		return err
	}
	endpoint := &f.endpoints[index]
	endpoint.ua = ua
	endpoint.server = server
	endpoint.serveDone = make(chan error, 1)
	server.OnMessage(func(request *sip.Request, transaction sip.ServerTransaction) {
		f.handleMessage(index, request, transaction)
	})
	server.OnInvite(func(request *sip.Request, transaction sip.ServerTransaction) {
		f.handleInvite(index, request, transaction)
	})
	server.OnAck(f.handleAck)
	server.OnBye(f.handleBye)
	ready := make(chan string, 1)
	serveContext := context.WithValue(ctx, sipgo.ListenReadyCtxKey, sipgo.ListenReadyFuncCtxValue(func(_ string, addr string) {
		ready <- addr
	}))
	go func() { endpoint.serveDone <- server.ListenAndServe(serveContext, "udp", listen) }()
	select {
	case endpoint.listenAddr = <-ready:
	case err := <-endpoint.serveDone:
		_ = ua.Close()
		return fmt.Errorf("SIP endpoint stopped before ready: %w", err)
	case <-ctx.Done():
		_ = ua.Close()
		return ctx.Err()
	}
	client, err := sipgo.NewClient(ua,
		sipgo.WithClientConnectionAddr(endpoint.listenAddr),
		sipgo.WithClientAddr(endpoint.listenAddr),
		sipgo.WithClientNAT(),
	)
	if err != nil {
		_ = ua.Close()
		return err
	}
	endpoint.client = client
	return nil
}

func endpointListenAddress(base string, index int) (string, error) {
	host, portText, err := net.SplitHostPort(base)
	if err != nil {
		return "", err
	}
	port, err := strconv.Atoi(portText)
	if err != nil {
		return "", err
	}
	if port != 0 {
		port += index
		if port > 65535 {
			return "", fmt.Errorf("SIP endpoint port range exceeds 65535")
		}
	}
	return net.JoinHostPort(host, strconv.Itoa(port)), nil
}

func (f *simulatedFleet) registerAll(ctx context.Context) error {
	return runIndices(ctx, f.cfg.devices, f.cfg.controlWorkers, f.cfg.registerRate, func(index int) error {
		requestContext, cancel := context.WithTimeout(ctx, 5*time.Second)
		defer cancel()
		return f.register(requestContext, index, false)
	})
}

func (f *simulatedFleet) register(ctx context.Context, index int, refresh bool) error {
	endpoint := &f.endpoints[index%len(f.endpoints)]
	deviceID := f.identities.deviceID(index)
	host, portText, err := net.SplitHostPort(endpoint.listenAddr)
	if err != nil {
		return err
	}
	port, err := strconv.Atoi(portText)
	if err != nil {
		return err
	}
	recipient, err := f.platformURI(false)
	if err != nil {
		return err
	}
	request := sip.NewRequest(sip.REGISTER, recipient)
	fromParams := sip.NewParams()
	fromParams.Add("tag", sip.GenerateTagN(16))
	request.AppendHeader(&sip.FromHeader{Address: sip.Uri{Scheme: "sip", User: deviceID, Host: f.cfg.domain}, Params: fromParams})
	request.AppendHeader(&sip.ToHeader{Address: sip.Uri{Scheme: "sip", User: deviceID, Host: f.cfg.domain}})
	request.AppendHeader(&sip.ContactHeader{Address: sip.Uri{Scheme: "sip", User: deviceID, Host: host, Port: port}})
	expires := sip.ExpiresHeader(uint32(f.cfg.registerExpiry / time.Second))
	request.AppendHeader(&expires)
	request.SetTransport("UDP")
	request.SetDestination(f.cfg.platformSIP)
	challenge, err := endpoint.client.Do(ctx, request)
	if err != nil {
		return err
	}
	if challenge.StatusCode != sip.StatusUnauthorized {
		return fmt.Errorf("device %d REGISTER challenge status %d", index, challenge.StatusCode)
	}
	response, err := endpoint.client.DoDigestAuth(ctx, request, challenge, sipgo.DigestAuth{Username: deviceID, Password: f.cfg.password})
	if err != nil {
		return err
	}
	if response.StatusCode != sip.StatusOK {
		return fmt.Errorf("device %d REGISTER status %d", index, response.StatusCode)
	}
	f.mutex.Lock()
	state := &f.states[index]
	state.registered = true
	state.refreshPending = false
	state.refreshAt = time.Now().Add(f.cfg.registerExpiry / 2)
	f.mutex.Unlock()
	if refresh {
		f.counters.registerRefresh.Add(1)
	} else {
		f.counters.registered.Add(1)
	}
	return nil
}

func (f *simulatedFleet) handleMessage(endpoint int, request *sip.Request, transaction sip.ServerTransaction) {
	if err := transaction.Respond(sip.NewResponseFromRequest(request, sip.StatusOK, "OK", nil)); err != nil {
		f.publishError(err)
		return
	}
	var query catalogQuery
	if err := xml.Unmarshal(request.Body(), &query); err != nil || query.CmdType != "Catalog" || query.SN <= 0 {
		return
	}
	index, ok := f.identities.deviceIndex(query.DeviceID)
	if !ok || index%len(f.endpoints) != endpoint {
		return
	}
	select {
	case f.catalogJobs <- catalogWork{endpoint: endpoint, device: index, sn: query.SN}:
	case <-f.ctx.Done():
	}
}

func (f *simulatedFleet) runCatalogWorker(ctx context.Context) {
	defer f.done.Done()
	for {
		select {
		case <-ctx.Done():
			return
		case work := <-f.catalogJobs:
			requestContext, cancel := context.WithTimeout(ctx, 3*time.Second)
			err := f.sendCatalog(requestContext, work)
			cancel()
			if err != nil {
				f.publishError(err)
				continue
			}
			f.mutex.Lock()
			if !f.states[work.device].cataloged {
				f.states[work.device].cataloged = true
				f.counters.catalogResponses.Add(1)
			}
			f.mutex.Unlock()
		}
	}
}

func (f *simulatedFleet) sendCatalog(ctx context.Context, work catalogWork) error {
	deviceID := f.identities.deviceID(work.device)
	response := catalogResponse{CmdType: "Catalog", SN: work.sn, DeviceID: deviceID, SumNum: 1}
	response.DeviceList.Num = 1
	response.DeviceList.Items = []catalogChannel{{
		DeviceID: f.identities.channelID(work.device), Name: "simulator camera", ParentID: deviceID, Status: "ON",
	}}
	body, err := xml.Marshal(response)
	if err != nil {
		return err
	}
	return f.sendMANSCDP(ctx, work.endpoint, work.device, append([]byte(xml.Header), body...))
}

func (f *simulatedFleet) initialKeepalives(ctx context.Context) error {
	return runIndices(ctx, f.cfg.devices, f.cfg.controlWorkers, 0, func(index int) error {
		requestContext, cancel := context.WithTimeout(ctx, 3*time.Second)
		defer cancel()
		return f.keepalive(requestContext, index)
	})
}

func (f *simulatedFleet) runHeartbeatScheduler(ctx context.Context) {
	defer f.done.Done()
	period := max(1, int(f.cfg.heartbeat/time.Second))
	ticker := time.NewTicker(time.Second)
	defer ticker.Stop()
	bucket := 0
	indices := make([]int, 0, (f.cfg.devices+period-1)/period)
	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
			indices = scheduleBucket(indices[:0], f.cfg.devices, period, bucket)
			for _, index := range indices {
				select {
				case <-ctx.Done():
					return
				case f.heartbeats <- index:
				}
			}
			bucket = (bucket + 1) % period
		}
	}
}

func (f *simulatedFleet) runRegistrationScheduler(ctx context.Context) {
	defer f.done.Done()
	refreshInterval := f.cfg.registerExpiry / 2
	ticker := time.NewTicker(min(time.Second, refreshInterval/2))
	defer ticker.Stop()
	indices := make([]int, 0, f.cfg.devices)
	for {
		select {
		case <-ctx.Done():
			return
		case now := <-ticker.C:
			indices = f.registrationRefreshesDue(indices[:0], now)
			for _, index := range indices {
				select {
				case <-ctx.Done():
					return
				case f.registrations <- index:
				}
			}
		}
	}
}

func (f *simulatedFleet) registrationRefreshesDue(destination []int, now time.Time) []int {
	f.mutex.Lock()
	defer f.mutex.Unlock()
	for index := range f.states {
		state := &f.states[index]
		if !state.registered || state.refreshPending || now.Before(state.refreshAt) {
			continue
		}
		state.refreshPending = true
		destination = append(destination, index)
	}
	return destination
}

func (f *simulatedFleet) runDeviceWorker(ctx context.Context) {
	defer f.done.Done()
	for {
		select {
		case <-ctx.Done():
			return
		case index := <-f.registrations:
			requestContext, cancel := context.WithTimeout(ctx, 5*time.Second)
			err := f.register(requestContext, index, true)
			cancel()
			if err != nil {
				f.mutex.Lock()
				f.states[index].refreshPending = false
				f.mutex.Unlock()
				f.counters.registerErrors.Add(1)
			}
		case index := <-f.heartbeats:
			requestContext, cancel := context.WithTimeout(ctx, 3*time.Second)
			err := f.keepalive(requestContext, index)
			cancel()
			if err != nil {
				f.counters.heartbeatErrors.Add(1)
			}
		}
	}
}

func (f *simulatedFleet) keepalive(ctx context.Context, index int) error {
	sn := f.states[index].heartbeatSN.Add(1) - 1
	body, err := xml.Marshal(keepaliveNotify{
		CmdType: "Keepalive", SN: int(sn), DeviceID: f.identities.deviceID(index), Status: "OK",
	})
	if err != nil {
		return err
	}
	if err := f.sendMANSCDP(ctx, index%len(f.endpoints), index, append([]byte(xml.Header), body...)); err != nil {
		return err
	}
	f.counters.heartbeats.Add(1)
	return nil
}

func (f *simulatedFleet) sendMANSCDP(ctx context.Context, endpointIndex, deviceIndex int, body []byte) error {
	recipient, err := f.platformURI(true)
	if err != nil {
		return err
	}
	deviceID := f.identities.deviceID(deviceIndex)
	request := sip.NewRequest(sip.MESSAGE, recipient)
	fromParams := sip.NewParams()
	fromParams.Add("tag", sip.GenerateTagN(16))
	request.AppendHeader(&sip.FromHeader{Address: sip.Uri{Scheme: "sip", User: deviceID, Host: f.cfg.domain}, Params: fromParams})
	request.AppendHeader(&sip.ToHeader{Address: sip.Uri{Scheme: "sip", User: f.cfg.platformID, Host: f.cfg.domain}})
	request.AppendHeader(sip.NewHeader("Content-Type", "Application/MANSCDP+xml"))
	request.SetBody(body)
	request.SetTransport("UDP")
	request.SetDestination(f.cfg.platformSIP)
	response, err := f.endpoints[endpointIndex].client.Do(ctx, request)
	if err != nil {
		return err
	}
	if !response.IsSuccess() {
		return fmt.Errorf("device %d MESSAGE rejected with SIP %d", deviceIndex, response.StatusCode)
	}
	return nil
}

func (f *simulatedFleet) handleInvite(endpointIndex int, request *sip.Request, transaction sip.ServerTransaction) {
	index, ok := f.identities.channelIndex(request.Recipient.User)
	if !ok || index%len(f.endpoints) != endpointIndex {
		_ = transaction.Respond(sip.NewResponseFromRequest(request, sip.StatusNotFound, "Not Found", nil))
		return
	}
	target, answer, err := parseLiveOffer(request.Body())
	if err != nil {
		_ = transaction.Respond(sip.NewResponseFromRequest(request, sip.StatusBadRequest, "Bad Request", nil))
		return
	}
	endpoint := &f.endpoints[endpointIndex]
	host, portText, err := net.SplitHostPort(endpoint.listenAddr)
	if err != nil {
		f.publishError(err)
		return
	}
	port, err := strconv.Atoi(portText)
	if err != nil {
		f.publishError(err)
		return
	}
	dialogUA := &sipgo.DialogUA{Client: endpoint.client, ContactHDR: sip.ContactHeader{Address: sip.Uri{
		Scheme: "sip", User: f.identities.deviceID(index), Host: host, Port: port,
	}}}
	dialog, err := dialogUA.ReadInvite(request, transaction)
	if err != nil {
		f.publishError(err)
		return
	}
	f.mutex.Lock()
	if f.states[index].dialog != nil {
		f.mutex.Unlock()
		_ = dialog.Respond(sip.StatusBusyHere, "Busy Here", nil)
		return
	}
	f.states[index].dialog = dialog
	f.states[index].dialogID = dialog.ID
	f.states[index].target = target
	f.dialogs[dialog.ID] = index
	f.mutex.Unlock()
	if !transaction.OnCancel(func(*sip.Request) { f.removeDialog(index, dialog.ID) }) {
		f.removeDialog(index, dialog.ID)
		return
	}
	f.counters.invites.Add(1)
	if err := dialog.Respond(sip.StatusTrying, "Trying", nil); err != nil {
		f.removeDialog(index, dialog.ID)
		f.publishError(err)
		return
	}
	if err := dialog.RespondSDP(answer); err != nil && err.Error() != "No ACK received" {
		f.removeDialog(index, dialog.ID)
		f.publishError(err)
	}
}

func (f *simulatedFleet) handleAck(request *sip.Request, transaction sip.ServerTransaction) {
	dialogID, err := sip.DialogIDFromRequestUAS(request)
	if err != nil {
		return
	}
	f.mutex.Lock()
	index, ok := f.dialogs[dialogID]
	if !ok {
		f.mutex.Unlock()
		return
	}
	state := &f.states[index]
	if state.streaming {
		f.mutex.Unlock()
		return
	}
	if err := state.dialog.ReadAck(request, transaction); err != nil {
		f.mutex.Unlock()
		return
	}
	if err := f.media.add(index, state.target); err != nil {
		f.mutex.Unlock()
		f.publishError(err)
		return
	}
	state.streaming = true
	f.mutex.Unlock()
	f.counters.acks.Add(1)
	f.counters.liveStarted.Add(1)
}

func (f *simulatedFleet) handleBye(request *sip.Request, transaction sip.ServerTransaction) {
	dialogID, err := sip.DialogIDFromRequestUAS(request)
	if err != nil {
		return
	}
	f.mutex.Lock()
	index, ok := f.dialogs[dialogID]
	if !ok {
		f.mutex.Unlock()
		_ = transaction.Respond(sip.NewResponseFromRequest(request, sip.StatusCallTransactionDoesNotExists, "Call/Transaction Does Not Exist", nil))
		return
	}
	dialog := f.states[index].dialog
	f.mutex.Unlock()
	if err := dialog.ReadBye(request, transaction); err != nil {
		return
	}
	f.removeDialog(index, dialogID)
	f.counters.byes.Add(1)
}

func (f *simulatedFleet) removeDialog(index int, dialogID string) {
	f.mutex.Lock()
	state := &f.states[index]
	if state.dialogID != dialogID {
		f.mutex.Unlock()
		return
	}
	streaming := state.streaming
	delete(f.dialogs, dialogID)
	state.dialog = nil
	state.dialogID = ""
	state.target = mediaTarget{}
	state.streaming = false
	f.mutex.Unlock()
	if streaming {
		f.media.remove(index)
		f.counters.liveStopped.Add(1)
	}
}

func (f *simulatedFleet) startLive(ctx context.Context) error {
	return runIndices(ctx, f.cfg.liveCount, f.cfg.controlWorkers, f.cfg.startRate, func(index int) error {
		requestContext, cancel := context.WithTimeout(ctx, 20*time.Second)
		defer cancel()
		response, err := f.control.startLive(requestContext, f.identities.deviceID(index), f.identities.channelID(index))
		if err != nil {
			return fmt.Errorf("device %d start live: %w", index, err)
		}
		return f.waitStreaming(requestContext, index, response)
	})
}

func (f *simulatedFleet) waitStreaming(ctx context.Context, index int, response liveStartResponse) error {
	ticker := time.NewTicker(time.Millisecond)
	defer ticker.Stop()
	for {
		f.mutex.Lock()
		streaming := f.states[index].streaming
		target := f.states[index].target
		f.mutex.Unlock()
		if streaming {
			if target.ssrc == response.SSRC && target.rtpPort == response.RTPPort {
				return nil
			}
			return fmt.Errorf("device %d live response does not match established dialog", index)
		}
		select {
		case <-ctx.Done():
			return fmt.Errorf("device %d ACK wait: %w", index, ctx.Err())
		case <-ticker.C:
		}
	}
}

func (f *simulatedFleet) stopLive(ctx context.Context) error {
	return runIndices(ctx, f.cfg.liveCount, f.cfg.controlWorkers, 0, func(index int) error {
		if err := f.control.stopLive(ctx, f.identities.deviceID(index), f.identities.channelID(index)); err != nil {
			return fmt.Errorf("device %d stop live: %w", index, err)
		}
		return nil
	})
}

func (f *simulatedFleet) waitCatalog(ctx context.Context) error {
	ticker := time.NewTicker(10 * time.Millisecond)
	defer ticker.Stop()
	for {
		if f.counters.catalogResponses.Load() == uint64(f.cfg.devices) {
			return nil
		}
		select {
		case err := <-f.errors:
			return err
		case <-ctx.Done():
			return fmt.Errorf("Catalog responses %d/%d: %w", f.counters.catalogResponses.Load(), f.cfg.devices, ctx.Err())
		case <-ticker.C:
		}
	}
}

func (f *simulatedFleet) publishError(err error) {
	select {
	case f.errors <- err:
	default:
	}
}

func (f *simulatedFleet) platformURI(withUser bool) (sip.Uri, error) {
	host, portText, err := net.SplitHostPort(f.cfg.platformSIP)
	if err != nil {
		return sip.Uri{}, err
	}
	port, err := strconv.Atoi(portText)
	if err != nil {
		return sip.Uri{}, err
	}
	uri := sip.Uri{Scheme: "sip", Host: host, Port: port}
	if withUser {
		uri.User = f.cfg.platformID
	}
	return uri, nil
}

func (f *simulatedFleet) report() {
	var memory runtime.MemStats
	runtime.ReadMemStats(&memory)
	f.logger.Info("simulator summary",
		"devices", f.cfg.devices,
		"registered", f.counters.registered.Load(),
		"register_refresh", f.counters.registerRefresh.Load(),
		"register_fail", f.counters.registerErrors.Load(),
		"catalog", f.counters.catalogResponses.Load(),
		"heartbeat_ok", f.counters.heartbeats.Load(),
		"heartbeat_fail", f.counters.heartbeatErrors.Load(),
		"invite", f.counters.invites.Load(),
		"ack", f.counters.acks.Load(),
		"bye", f.counters.byes.Load(),
		"live_active", f.counters.liveStarted.Load()-f.counters.liveStopped.Load(),
		"rtp_packets", f.media.counters.packets.Load(),
		"rtp_bytes", f.media.counters.bytes.Load(),
		"rtp_dropped", f.media.counters.dropped.Load(),
		"send_errors", f.media.counters.sendErrors.Load(),
		"phase_drops", f.media.counters.phaseDrops.Load(),
		"goroutines", runtime.NumGoroutine(),
		"heap_bytes", memory.HeapAlloc,
		"gc", memory.NumGC,
	)
}

func (f *simulatedFleet) close() {
	f.cancel()
	_ = f.media.stop()
	for index := range f.endpoints {
		_ = f.endpoints[index].ua.Close()
	}
	f.done.Wait()
	for index := range f.endpoints {
		select {
		case <-f.endpoints[index].serveDone:
		case <-time.After(2 * time.Second):
		}
	}
}

func runFleet(ctx context.Context, cfg config, source *sharedMediaSource, logger *slog.Logger) error {
	fleet, err := newSimulatedFleet(ctx, cfg, source, logger)
	if err != nil {
		return err
	}
	defer fleet.close()
	if err := fleet.registerAll(ctx); err != nil {
		return err
	}
	catalogContext, cancel := context.WithTimeout(ctx, 30*time.Second)
	err = fleet.waitCatalog(catalogContext)
	cancel()
	if err != nil {
		return err
	}
	if err := fleet.initialKeepalives(ctx); err != nil {
		return err
	}
	fleet.done.Add(1)
	go fleet.runHeartbeatScheduler(fleet.ctx)
	if err := fleet.startLive(ctx); err != nil {
		return err
	}
	fleet.report()
	reportTicker := time.NewTicker(time.Second)
	defer reportTicker.Stop()
	liveTimer := time.NewTimer(cfg.liveDuration)
	defer liveTimer.Stop()
waitLoop:
	for {
		select {
		case <-ctx.Done():
			return ctx.Err()
		case err := <-fleet.errors:
			return err
		case <-reportTicker.C:
			fleet.report()
		case <-liveTimer.C:
			break waitLoop
		}
	}
	stopContext, stopCancel := context.WithTimeout(context.Background(), 30*time.Second)
	err = fleet.stopLive(stopContext)
	stopCancel()
	if err != nil {
		return err
	}
	fleet.report()
	return nil
}
