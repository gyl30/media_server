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
		case "/gb28181/create":
			close(createStarted)
			select {
			case <-request.Context().Done():
			case <-releaseCreate:
				writeHTTPError(writer, http.StatusInternalServerError, "released")
			}
		case "/gb28181/delete":
			writeJSON(writer, http.StatusOK, map[string]string{"result": "ok"})
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
	server := newInfrastructureServer(testConfig(), mediaRegistry, slog.New(slog.NewTextHandler(io.Discard, nil)))
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
	server := newInfrastructureServer(testConfig(), mediaRegistry, slog.New(slog.NewTextHandler(io.Discard, nil)))
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
	server := newInfrastructureServer(testConfig(), mediaRegistry, slog.New(slog.NewTextHandler(io.Discard, nil)))
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
