package main

import (
	"context"
	"encoding/json"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/emiago/sipgo/sip"
	"github.com/google/uuid"
)

func TestLiveControlHTTPStartCancellationCancelsMediaCreate(t *testing.T) {
	device := startLiveTestDevice(t, func(request *sip.Request) ([]byte, int) {
		return []byte(strings.ReplaceAll(string(request.Body()), "a=recvonly", "a=sendonly")), sip.StatusOK
	})
	platform, _ := startRegistrar(t, testConfig())
	registerLiveTestDevice(t, platform, device.addr)
	createStarted := make(chan struct{})
	releaseCreate := make(chan struct{}, 1)
	mediaServer := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
		writer.Header().Set("Content-Type", "application/json")
		switch request.URL.Path {
		case "/gb28181/receiver/create":
			close(createStarted)
			select {
			case <-request.Context().Done():
			case <-releaseCreate:
				writeHTTPError(writer, http.StatusInternalServerError, "released")
			}
		case "/gb28181/receiver/delete":
			writer.WriteHeader(http.StatusNoContent)
		default:
			http.NotFound(writer, request)
		}
	}))
	t.Cleanup(mediaServer.Close)
	mediaRegistry := newMediaServerRegistry()
	if err := mediaRegistry.register(mediaServerRegistration{
		ServerID: "media-1", InstanceID: "instance-a", ControlURL: mediaServer.URL, MediaIP: "127.0.0.1",
	}, time.Now()); err != nil {
		t.Fatalf("media registry register error = %v", err)
	}
	allocator, err := newSSRCAllocator(platform.cfg.sipDomain)
	if err != nil {
		t.Fatalf("newSSRCAllocator() error = %v", err)
	}
	live := newLiveService(platform, mediaRegistry, newMediaServerHTTPClient(time.Second), allocator, slog.New(slog.NewTextHandler(io.Discard, nil)))
	server := newTestInfrastructureServer(t, testConfig(), mediaRegistry, slog.New(slog.NewTextHandler(io.Discard, nil)))
	server.live = live
	requestContext, cancel := context.WithCancel(t.Context())
	request := httptest.NewRequestWithContext(requestContext, http.MethodPost, "/internal/live/start", strings.NewReader(`{"device_id":"`+testDeviceID+`","channel_id":"`+testChannelID+`"}`))
	request.Header.Set("Content-Type", "application/json")
	done := make(chan struct{})
	go func() {
		server.handleLiveStart(httptest.NewRecorder(), request)
		close(done)
	}()
	select {
	case <-createStarted:
	case <-time.After(time.Second):
		t.Fatal("media create did not start")
	}
	cancel()
	select {
	case <-done:
	case <-time.After(200 * time.Millisecond):
		releaseCreate <- struct{}{}
		<-done
		t.Fatal("canceling the live-control request did not stop live creation")
	}
	select {
	case releaseCreate <- struct{}{}:
	default:
	}
	if live.len() != 0 || allocator.activeCount() != 0 {
		t.Fatalf("canceled start leaked live=%d ssrc=%d", live.len(), allocator.activeCount())
	}
}

func TestLiveControlHTTPStartAndStop(t *testing.T) {
	device := startLiveTestDevice(t, func(request *sip.Request) ([]byte, int) {
		return []byte(strings.ReplaceAll(string(request.Body()), "a=recvonly", "a=sendonly")), sip.StatusOK
	})
	platform, _ := startRegistrar(t, testConfig())
	registerLiveTestDevice(t, platform, device.addr)
	mediaRegistry, _, _, deletes := startLiveTestMediaServer(t)
	allocator, err := newSSRCAllocator(platform.cfg.sipDomain)
	if err != nil {
		t.Fatalf("newSSRCAllocator() error = %v", err)
	}
	live := newLiveService(platform, mediaRegistry, newMediaServerHTTPClient(time.Second), allocator, slog.New(slog.NewTextHandler(io.Discard, nil)))
	server := newTestInfrastructureServer(t, testConfig(), mediaRegistry, slog.New(slog.NewTextHandler(io.Discard, nil)))
	server.live = live
	httpServer := httptest.NewServer(server.handler())
	defer httpServer.Close()

	response := postJSON(t, httpServer.Client(), httpServer.URL+"/internal/live/start", liveControlRequest{
		DeviceID: testDeviceID, ChannelID: testChannelID,
	})
	if response.StatusCode != http.StatusCreated {
		t.Fatalf("start status = %d body = %s", response.StatusCode, readBody(t, response))
	}
	var started struct {
		Result     string    `json:"result"`
		StreamID   string    `json:"stream_id"`
		StreamName string    `json:"stream_name"`
		State      liveState `json:"state"`
		SSRC       uint32    `json:"ssrc"`
		RTPPort    uint16    `json:"rtp_port"`
	}
	if err := json.NewDecoder(response.Body).Decode(&started); err != nil {
		response.Body.Close()
		t.Fatalf("Decode() error = %v", err)
	}
	response.Body.Close()
	if started.Result != "ok" || started.StreamName != "gb/"+testDeviceID+"/"+testChannelID || started.State != liveStreaming || started.SSRC == 0 || started.RTPPort == 0 {
		t.Fatalf("start response = %+v", started)
	}
	parsedStreamID, err := uuid.Parse(started.StreamID)
	if err != nil || parsedStreamID.Version() != 4 || parsedStreamID.String() != started.StreamID {
		t.Fatalf("stream_id = %q, error = %v", started.StreamID, err)
	}
	select {
	case <-device.acks:
	case <-time.After(2 * time.Second):
		t.Fatal("ACK was not received")
	}

	response = postJSON(t, httpServer.Client(), httpServer.URL+"/internal/live/stop", liveControlRequest{
		DeviceID: testDeviceID, ChannelID: testChannelID,
	})
	if response.StatusCode != http.StatusOK {
		t.Fatalf("stop status = %d body = %s", response.StatusCode, readBody(t, response))
	}
	response.Body.Close()
	select {
	case <-device.byes:
	case <-time.After(2 * time.Second):
		t.Fatal("BYE was not received")
	}
	if live.len() != 0 || allocator.activeCount() != 0 || deletes.Load() != 1 {
		t.Fatalf("cleanup live=%d ssrc=%d deletes=%d", live.len(), allocator.activeCount(), deletes.Load())
	}
}

func TestLiveControlHTTPRejectsInvalidAndMissingLive(t *testing.T) {
	platform := newTestSIPServer(t, "127.0.0.1:0")
	mediaRegistry := newMediaServerRegistry()
	allocator, err := newSSRCAllocator(platform.cfg.sipDomain)
	if err != nil {
		t.Fatalf("newSSRCAllocator() error = %v", err)
	}
	live := newLiveService(platform, mediaRegistry, newMediaServerHTTPClient(time.Second), allocator, slog.New(slog.NewTextHandler(io.Discard, nil)))
	server := newTestInfrastructureServer(t, testConfig(), mediaRegistry, slog.New(slog.NewTextHandler(io.Discard, nil)))
	server.live = live
	httpServer := httptest.NewServer(server.handler())
	defer httpServer.Close()

	response := postJSON(t, httpServer.Client(), httpServer.URL+"/internal/live/start", liveControlRequest{
		DeviceID: "bad", ChannelID: testChannelID,
	})
	assertHTTPError(t, response, http.StatusBadRequest, "invalid_request")

	response = postJSON(t, httpServer.Client(), httpServer.URL+"/internal/live/stop", liveControlRequest{
		DeviceID: testDeviceID, ChannelID: testChannelID,
	})
	assertHTTPError(t, response, http.StatusNotFound, "live_not_found")
}

func TestLiveControlRetainsAmbiguousCreateUntilCleanupSucceeds(t *testing.T) {
	platform := newTestSIPServer(t, "127.0.0.1:0")
	registerLiveTestDevice(t, platform, "127.0.0.1:5062")
	mediaRegistry := newMediaServerRegistry()
	allocator, err := newSSRCAllocator(platform.cfg.sipDomain)
	if err != nil {
		t.Fatalf("newSSRCAllocator() error = %v", err)
	}
	deleteStatuses := make(chan int, 2)
	deleteStatuses <- http.StatusNotFound
	deleteStatuses <- http.StatusNoContent
	deletes := make(chan string, 2)
	var infrastructure *infrastructureServer
	mediaServer := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
		switch request.URL.Path {
		case "/gb28181/receiver/create":
			var command struct {
				StreamID   string `json:"stream_id"`
				StreamName string `json:"stream_name"`
			}
			if err := json.NewDecoder(request.Body).Decode(&command); err != nil {
				t.Errorf("decode create: %v", err)
			}
			if _, err := infrastructure.runtimes.apply(observedRuntime{
				Kind: "source", ServerID: "media-1", InstanceID: "instance-a",
				StreamID: command.StreamID, StreamName: command.StreamName,
				Protocol: "gb28181", State: "starting", Stage: "receiving",
			}); err != nil {
				t.Errorf("apply starting runtime error = %v", err)
			}
			writer.Header().Set("Content-Type", "text/plain")
			writer.WriteHeader(http.StatusCreated)
		case "/gb28181/receiver/delete":
			var command struct {
				StreamID string `json:"stream_id"`
			}
			if err := json.NewDecoder(request.Body).Decode(&command); err != nil {
				t.Errorf("decode delete: %v", err)
			}
			deletes <- command.StreamID
			if status := <-deleteStatuses; status == http.StatusNoContent {
				writer.WriteHeader(status)
			} else {
				writeHTTPError(writer, status, "operation_failed")
			}
		default:
			http.NotFound(writer, request)
		}
	}))
	defer mediaServer.Close()
	if err := mediaRegistry.register(mediaServerRegistration{
		ServerID: "media-1", InstanceID: "instance-a", ControlURL: mediaServer.URL, MediaIP: "127.0.0.1",
	}, time.Now()); err != nil {
		t.Fatalf("register media server error = %v", err)
	}
	live := newLiveService(platform, mediaRegistry, newMediaServerHTTPClient(time.Second), allocator,
		slog.New(slog.NewTextHandler(io.Discard, nil)))
	infrastructure = newTestInfrastructureServer(t, testConfig(), mediaRegistry,
		slog.New(slog.NewTextHandler(io.Discard, nil)))
	infrastructure.live = live

	response := sourceRequest(t, infrastructure.handler(), http.MethodPost, "/internal/live/start",
		`{"device_id":"`+testDeviceID+`","channel_id":"`+testChannelID+`"}`, "application/json")
	if response.Code != http.StatusBadGateway {
		t.Fatalf("start status/body = %d %s", response.Code, response.Body.String())
	}
	firstStreamID := <-deletes
	if live.len() != 1 || allocator.activeCount() != 1 {
		t.Fatalf("retained live/ssrc = %d/%d", live.len(), allocator.activeCount())
	}
	view, ok := live.live(testDeviceID, testChannelID)
	live.mu.Lock()
	retained, retainedOK := live.sessions[liveKey{deviceID: testDeviceID, channelID: testChannelID}]
	internalState := liveState("")
	if retainedOK {
		internalState = retained.state
	}
	live.mu.Unlock()
	if !ok || !retainedOK || view.streamID != firstStreamID || view.state != liveStopping || internalState != liveCleanupPending {
		t.Fatalf("retained live = %+v, %v", view, ok)
	}
	observed := infrastructure.runtimes.snapshot()
	if len(observed) != 1 || observed[0].StreamID != firstStreamID || observed[0].State != "starting" {
		t.Fatalf("observed after ambiguous create = %+v", observed)
	}

	response = sourceRequest(t, infrastructure.handler(), http.MethodPost, "/internal/live/stop",
		`{"device_id":"`+testDeviceID+`","channel_id":"`+testChannelID+`"}`, "application/json")
	if response.Code != http.StatusOK {
		t.Fatalf("retry stop status/body = %d %s", response.Code, response.Body.String())
	}
	if secondStreamID := <-deletes; secondStreamID != firstStreamID {
		t.Fatalf("retry stream ID = %q, first = %q", secondStreamID, firstStreamID)
	}
	if live.len() != 0 || allocator.activeCount() != 0 {
		t.Fatalf("cleaned live/ssrc = %d/%d", live.len(), allocator.activeCount())
	}
	observed = infrastructure.runtimes.snapshot()
	if len(observed) != 1 || observed[0].State != "stopped" || observed[0].EndReason != "requested" {
		t.Fatalf("observed after cleanup retry = %+v", observed)
	}
}

func TestLiveControlClosesObservedRuntimeAfterCreateCompensation(t *testing.T) {
	platform := newTestSIPServer(t, "127.0.0.1:0")
	registerLiveTestDevice(t, platform, "127.0.0.1:5062")
	mediaRegistry := newMediaServerRegistry()
	allocator, err := newSSRCAllocator(platform.cfg.sipDomain)
	if err != nil {
		t.Fatalf("newSSRCAllocator() error = %v", err)
	}
	deleted := make(chan string, 1)
	var infrastructure *infrastructureServer
	mediaServer := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
		switch request.URL.Path {
		case "/gb28181/receiver/create":
			var command struct {
				StreamID   string `json:"stream_id"`
				StreamName string `json:"stream_name"`
			}
			if err := json.NewDecoder(request.Body).Decode(&command); err != nil {
				t.Errorf("decode create: %v", err)
			}
			if _, err := infrastructure.runtimes.apply(observedRuntime{
				Kind: "source", ServerID: "media-1", InstanceID: "instance-a",
				StreamID: command.StreamID, StreamName: command.StreamName,
				Protocol: "gb28181", State: "starting", Stage: "receiving",
			}); err != nil {
				t.Errorf("apply starting runtime error = %v", err)
			}
			writer.Header().Set("Content-Type", "text/plain")
			writer.WriteHeader(http.StatusCreated)
		case "/gb28181/receiver/delete":
			var command struct {
				StreamID string `json:"stream_id"`
			}
			if err := json.NewDecoder(request.Body).Decode(&command); err != nil {
				t.Errorf("decode delete: %v", err)
			}
			deleted <- command.StreamID
			writer.WriteHeader(http.StatusNoContent)
		default:
			http.NotFound(writer, request)
		}
	}))
	defer mediaServer.Close()
	if err := mediaRegistry.register(mediaServerRegistration{
		ServerID: "media-1", InstanceID: "instance-a", ControlURL: mediaServer.URL, MediaIP: "127.0.0.1",
	}, time.Now()); err != nil {
		t.Fatalf("register media server error = %v", err)
	}
	live := newLiveService(platform, mediaRegistry, newMediaServerHTTPClient(time.Second), allocator,
		slog.New(slog.NewTextHandler(io.Discard, nil)))
	infrastructure = newTestInfrastructureServer(t, testConfig(), mediaRegistry,
		slog.New(slog.NewTextHandler(io.Discard, nil)))
	infrastructure.live = live

	response := sourceRequest(t, infrastructure.handler(), http.MethodPost, "/internal/live/start",
		`{"device_id":"`+testDeviceID+`","channel_id":"`+testChannelID+`"}`, "application/json")
	if response.Code != http.StatusBadGateway {
		t.Fatalf("start status/body = %d %s", response.Code, response.Body.String())
	}
	streamID := <-deleted
	if live.len() != 0 || allocator.activeCount() != 0 {
		t.Fatalf("cleanup live/ssrc = %d/%d", live.len(), allocator.activeCount())
	}
	observed := infrastructure.runtimes.snapshot()
	if len(observed) != 1 || observed[0].StreamID != streamID || observed[0].State != "stopped" ||
		observed[0].EndReason != "requested" {
		t.Fatalf("observed after create compensation = %+v", observed)
	}
}

func TestLiveControlRuntimeStopWinsCreateCompensationFailure(t *testing.T) {
	platform := newTestSIPServer(t, "127.0.0.1:0")
	registerLiveTestDevice(t, platform, "127.0.0.1:5062")
	mediaRegistry := newMediaServerRegistry()
	allocator, err := newSSRCAllocator(platform.cfg.sipDomain)
	if err != nil {
		t.Fatalf("newSSRCAllocator() error = %v", err)
	}
	created := make(chan struct {
		streamID   string
		streamName string
	}, 1)
	deleteStarted := make(chan struct{})
	releaseDelete := make(chan struct{})
	var infrastructure *infrastructureServer
	mediaServer := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
		switch request.URL.Path {
		case "/gb28181/receiver/create":
			var command struct {
				StreamID   string `json:"stream_id"`
				StreamName string `json:"stream_name"`
			}
			if err := json.NewDecoder(request.Body).Decode(&command); err != nil {
				t.Errorf("decode create: %v", err)
			}
			created <- struct {
				streamID   string
				streamName string
			}{streamID: command.StreamID, streamName: command.StreamName}
			if _, err := infrastructure.runtimes.apply(observedRuntime{
				Kind: "source", ServerID: "media-1", InstanceID: "instance-a",
				StreamID: command.StreamID, StreamName: command.StreamName,
				Protocol: "gb28181", State: "starting", Stage: "receiving",
			}); err != nil {
				t.Errorf("apply starting runtime error = %v", err)
			}
			writer.Header().Set("Content-Type", "text/plain")
			writer.WriteHeader(http.StatusCreated)
		case "/gb28181/receiver/delete":
			close(deleteStarted)
			<-releaseDelete
			writeHTTPError(writer, http.StatusInternalServerError, "operation_failed")
		default:
			http.NotFound(writer, request)
		}
	}))
	defer mediaServer.Close()
	if err := mediaRegistry.register(mediaServerRegistration{
		ServerID: "media-1", InstanceID: "instance-a", ControlURL: mediaServer.URL, MediaIP: "127.0.0.1",
	}, time.Now()); err != nil {
		t.Fatalf("register media server error = %v", err)
	}
	live := newLiveService(platform, mediaRegistry, newMediaServerHTTPClient(time.Second), allocator,
		slog.New(slog.NewTextHandler(io.Discard, nil)))
	infrastructure = newTestInfrastructureServer(t, testConfig(), mediaRegistry,
		slog.New(slog.NewTextHandler(io.Discard, nil)))
	infrastructure.live = live
	startResult := make(chan *httptest.ResponseRecorder, 1)
	go func() {
		startResult <- sourceRequest(t, infrastructure.handler(), http.MethodPost, "/internal/live/start",
			`{"device_id":"`+testDeviceID+`","channel_id":"`+testChannelID+`"}`, "application/json")
	}()
	command := <-created
	select {
	case <-deleteStarted:
	case <-time.After(time.Second):
		t.Fatal("create compensation did not start")
	}
	stopResult := make(chan *httptest.ResponseRecorder, 1)
	go func() {
		body := `{"kind":"source","server_id":"media-1","instance_id":"instance-a",` +
			`"stream_id":"` + command.streamID + `","stream_name":"` + command.streamName + `",` +
			`"protocol":"gb28181","state":"stopped","end_reason":"remote"}`
		stopResult <- sourceRequest(t, infrastructure.handler(), http.MethodPost, "/internal/runtime-events", body, "application/json")
	}()
	deadline := time.Now().Add(time.Second)
	for {
		live.mu.Lock()
		session := live.sessions[liveKey{deviceID: testDeviceID, channelID: testChannelID}]
		remoteStopped := session != nil && !session.deleteMedia
		live.mu.Unlock()
		if remoteStopped {
			break
		}
		if time.Now().After(deadline) {
			t.Fatal("runtime stop did not reach live session")
		}
		time.Sleep(time.Millisecond)
	}
	close(releaseDelete)
	if response := <-startResult; response.Code != http.StatusBadGateway {
		t.Fatalf("start status/body = %d %s", response.Code, response.Body.String())
	}
	if response := <-stopResult; response.Code != http.StatusNoContent {
		t.Fatalf("runtime stop status/body = %d %s", response.Code, response.Body.String())
	}
	if live.len() != 0 || allocator.activeCount() != 0 {
		t.Fatalf("runtime stop retained live/ssrc = %d/%d", live.len(), allocator.activeCount())
	}
	observed := infrastructure.runtimes.snapshot()
	if len(observed) != 1 || observed[0].StreamID != command.streamID || observed[0].State != "stopped" ||
		observed[0].EndReason != "remote" {
		t.Fatalf("observed after runtime stop = %+v", observed)
	}
}

func TestLiveControlRetainsInviteFailureUntilCleanupSucceeds(t *testing.T) {
	device := startLiveTestDevice(t, func(*sip.Request) ([]byte, int) {
		return nil, sip.StatusBusyHere
	})
	platform, _ := startRegistrar(t, testConfig())
	registerLiveTestDevice(t, platform, device.addr)
	mediaRegistry := newMediaServerRegistry()
	allocator, err := newSSRCAllocator(platform.cfg.sipDomain)
	if err != nil {
		t.Fatalf("newSSRCAllocator() error = %v", err)
	}
	deleted := make(chan string, 2)
	deleteStatuses := make(chan int, 2)
	deleteStatuses <- http.StatusServiceUnavailable
	deleteStatuses <- http.StatusNoContent
	var infrastructure *infrastructureServer
	mediaServer := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
		switch request.URL.Path {
		case "/gb28181/receiver/create":
			var command struct {
				StreamID   string `json:"stream_id"`
				StreamName string `json:"stream_name"`
			}
			if err := json.NewDecoder(request.Body).Decode(&command); err != nil {
				t.Errorf("decode create: %v", err)
			}
			if _, err := infrastructure.runtimes.apply(observedRuntime{
				Kind: "source", ServerID: "media-1", InstanceID: "instance-a",
				StreamID: command.StreamID, StreamName: command.StreamName,
				Protocol: "gb28181", State: "starting", Stage: "receiving",
			}); err != nil {
				t.Errorf("apply starting runtime error = %v", err)
			}
			writeJSON(writer, http.StatusCreated, map[string]any{
				"rtp_port": 40000, "rtcp_port": 40001,
			})
		case "/gb28181/receiver/delete":
			var command struct {
				StreamID string `json:"stream_id"`
			}
			if err := json.NewDecoder(request.Body).Decode(&command); err != nil {
				t.Errorf("decode delete: %v", err)
			}
			deleted <- command.StreamID
			if status := <-deleteStatuses; status == http.StatusNoContent {
				writer.WriteHeader(status)
			} else {
				writeHTTPError(writer, status, "operation_failed")
			}
		default:
			http.NotFound(writer, request)
		}
	}))
	defer mediaServer.Close()
	if err := mediaRegistry.register(mediaServerRegistration{
		ServerID: "media-1", InstanceID: "instance-a", ControlURL: mediaServer.URL, MediaIP: "127.0.0.1",
	}, time.Now()); err != nil {
		t.Fatalf("register media server error = %v", err)
	}
	live := newLiveService(platform, mediaRegistry, newMediaServerHTTPClient(time.Second), allocator,
		slog.New(slog.NewTextHandler(io.Discard, nil)))
	infrastructure = newTestInfrastructureServer(t, testConfig(), mediaRegistry,
		slog.New(slog.NewTextHandler(io.Discard, nil)))
	infrastructure.live = live

	response := sourceRequest(t, infrastructure.handler(), http.MethodPost, "/internal/live/start",
		`{"device_id":"`+testDeviceID+`","channel_id":"`+testChannelID+`"}`, "application/json")
	if response.Code != http.StatusBadGateway {
		t.Fatalf("start status/body = %d %s", response.Code, response.Body.String())
	}
	streamID := <-deleted
	view, ok := live.live(testDeviceID, testChannelID)
	if !ok || view.streamID != streamID || view.state != liveStopping || live.len() != 1 || allocator.activeCount() != 1 {
		t.Fatalf("retained cleanup live=%+v ok=%v count=%d ssrc=%d", view, ok, live.len(), allocator.activeCount())
	}
	observed := infrastructure.runtimes.snapshot()
	if len(observed) != 1 || observed[0].StreamID != streamID || observed[0].State != "starting" {
		t.Fatalf("observed after failed cleanup = %+v", observed)
	}
	response = sourceRequest(t, infrastructure.handler(), http.MethodPost, "/internal/live/stop",
		`{"device_id":"`+testDeviceID+`","channel_id":"`+testChannelID+`"}`, "application/json")
	if response.Code != http.StatusOK {
		t.Fatalf("stop status/body = %d %s", response.Code, response.Body.String())
	}
	if retryStreamID := <-deleted; retryStreamID != streamID {
		t.Fatalf("retry stream ID = %q, want %q", retryStreamID, streamID)
	}
	if live.len() != 0 || allocator.activeCount() != 0 {
		t.Fatalf("retry cleanup live/ssrc = %d/%d", live.len(), allocator.activeCount())
	}
	observed = infrastructure.runtimes.snapshot()
	if len(observed) != 1 || observed[0].StreamID != streamID || observed[0].State != "stopped" ||
		observed[0].EndReason != "requested" {
		t.Fatalf("observed after cleanup retry = %+v", observed)
	}
}

func TestLiveControlRetriesUnconfirmedRuntimeDelete(t *testing.T) {
	platform := newTestSIPServer(t, "127.0.0.1:0")
	mediaRegistry := newMediaServerRegistry()
	deleteStatuses := make(chan int, 2)
	deleteStatuses <- http.StatusNotFound
	deleteStatuses <- http.StatusInternalServerError
	deletes := make(chan string, 2)
	mediaServer := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
		var command struct {
			StreamID string `json:"stream_id"`
		}
		if err := json.NewDecoder(request.Body).Decode(&command); err != nil {
			t.Errorf("decode delete: %v", err)
		}
		deletes <- command.StreamID
		if status := <-deleteStatuses; status == http.StatusNoContent {
			writer.WriteHeader(status)
		} else {
			writeHTTPError(writer, status, "operation_failed")
		}
	}))
	defer mediaServer.Close()
	registration := mediaServerRegistration{
		ServerID: "media-1", InstanceID: "instance-a", ControlURL: mediaServer.URL, MediaIP: "127.0.0.1",
	}
	if err := mediaRegistry.register(registration, time.Now()); err != nil {
		t.Fatalf("register media server error = %v", err)
	}
	allocator, err := newSSRCAllocator(platform.cfg.sipDomain)
	if err != nil {
		t.Fatalf("newSSRCAllocator() error = %v", err)
	}
	ssrc, err := allocator.acquire()
	if err != nil {
		t.Fatalf("acquire SSRC error = %v", err)
	}
	live := newLiveService(platform, mediaRegistry, newMediaServerHTTPClient(time.Second), allocator,
		slog.New(slog.NewTextHandler(io.Discard, nil)))
	streamID := uuid.NewString()
	streamName := "gb/" + testDeviceID + "/" + testChannelID
	_, cancel := context.WithCancel(context.Background())
	session := &liveSession{
		key: liveKey{deviceID: testDeviceID, channelID: testChannelID}, streamID: streamID, streamName: streamName,
		server: mediaServerInstance{serverID: registration.ServerID, instanceID: registration.InstanceID,
			controlURL: registration.ControlURL},
		endpoint: gb28181ReceiverEndpoint{rtpPort: 40000}, ssrc: ssrc, state: liveStreaming, cancel: cancel,
		established: make(chan struct{}), done: make(chan struct{}), deleteMedia: true,
	}
	close(session.established)
	live.sessions[session.key] = session
	infrastructure := newTestInfrastructureServer(t, testConfig(), mediaRegistry,
		slog.New(slog.NewTextHandler(io.Discard, nil)))
	infrastructure.live = live
	if _, err := infrastructure.runtimes.apply(observedRuntime{
		Kind: "source", ServerID: registration.ServerID, InstanceID: registration.InstanceID,
		StreamID: streamID, StreamName: streamName, Protocol: "gb28181",
		State: "streaming", Stage: "streaming",
	}); err != nil {
		t.Fatalf("apply runtime error = %v", err)
	}

	stop := func(want int) *httptest.ResponseRecorder {
		response := sourceRequest(t, infrastructure.handler(), http.MethodPost, "/internal/live/stop",
			`{"device_id":"`+testDeviceID+`","channel_id":"`+testChannelID+`"}`, "application/json")
		if response.Code != want {
			t.Fatalf("stop status/body = %d %s, want %d", response.Code, response.Body.String(), want)
		}
		return response
	}
	stop(http.StatusBadGateway)
	if firstStreamID := <-deletes; firstStreamID != streamID {
		t.Fatalf("first delete stream ID = %q", firstStreamID)
	}
	view, ok := live.live(testDeviceID, testChannelID)
	live.mu.Lock()
	retained, retainedOK := live.sessions[liveKey{deviceID: testDeviceID, channelID: testChannelID}]
	internalState := liveState("")
	if retainedOK {
		internalState = retained.state
	}
	live.mu.Unlock()
	if !ok || !retainedOK || view.streamID != streamID || view.state != liveStopping || internalState != liveCleanupPending ||
		allocator.activeCount() != 1 {
		t.Fatalf("retained live = %+v, %v, ssrc=%d", view, ok, allocator.activeCount())
	}
	if observed, ok := infrastructure.runtimes.byStreamID[streamID]; !ok || observed.State != "streaming" {
		t.Fatalf("observed after failed delete = %+v, %v", observed, ok)
	}

	stop(http.StatusOK)
	if secondStreamID := <-deletes; secondStreamID != streamID {
		t.Fatalf("second delete stream ID = %q", secondStreamID)
	}
	if live.len() != 0 || allocator.activeCount() != 0 {
		t.Fatalf("cleaned live/ssrc = %d/%d", live.len(), allocator.activeCount())
	}
	if observed, ok := infrastructure.runtimes.byStreamID[streamID]; !ok || observed.State != "stopped" {
		t.Fatalf("observed after retry = %+v, %v", observed, ok)
	}
}
