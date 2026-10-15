package main

import (
	"encoding/json"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func testInfrastructure(t *testing.T) *infrastructureServer {
	t.Helper()
	sources, err := openSourceStore(t.Context(), filepath.Join(t.TempDir(), "signaling.db"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = sources.close() })
	cfg, err := parseConfig(nil)
	if err != nil {
		t.Fatal(err)
	}
	logger := slog.New(slog.NewTextHandler(io.Discard, nil))
	devices, err := newDeviceStore(t.Context(), sources.db)
	if err != nil {
		t.Fatal(err)
	}
	server, err := newSIPServer(cfg, devices, logger)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(server.close)
	ssrcs, err := newSSRCAllocator(cfg.sipDomain)
	if err != nil {
		t.Fatal(err)
	}
	media := newMediaServerHTTPClient(cfg.mediaServer, time.Second)
	live := newLiveService(server, media, ssrcs, time.Second, time.Second, logger)
	return newInfrastructureServer("127.0.0.1:0", sources, live, media, logger)
}

func apiRequest(t *testing.T, handler http.Handler, method, path, body string, status int) *httptest.ResponseRecorder {
	t.Helper()
	request := httptest.NewRequest(method, path, strings.NewReader(body))
	request.Header.Set("Content-Type", "application/json")
	response := httptest.NewRecorder()
	handler.ServeHTTP(response, request)
	if response.Code != status {
		t.Fatalf("%s %s: HTTP %d want %d: %s", method, path, response.Code, status, response.Body.String())
	}
	return response
}

func TestDeviceHTTPPersistsOfflineDevice(t *testing.T) {
	s := testInfrastructure(t)
	handler := s.handler()
	body := `{"device_id":"34020000001320000001","name":"大门摄像机"}`
	response := apiRequest(t, handler, "POST", "/api/devices", body, http.StatusCreated)
	var device map[string]any
	if err := json.Unmarshal(response.Body.Bytes(), &device); err != nil {
		t.Fatal(err)
	}
	if device["name"] != "大门摄像机" || device["online"] != false {
		t.Fatalf("device: %v", device)
	}
	apiRequest(t, handler, "POST", "/api/devices", body, http.StatusConflict)
	apiRequest(t, handler, "GET", "/api/devices/34020000001320000001", "", http.StatusOK)
	apiRequest(t, handler, "GET", "/api/devices/34020000001320000002", "", http.StatusNotFound)
	channels := apiRequest(t, handler, "GET", "/api/devices/34020000001320000001/channels", "", http.StatusOK)
	if strings.TrimSpace(channels.Body.String()) != `{"channels":[]}` {
		t.Fatalf("offline channels: %s", channels.Body.String())
	}
	response = apiRequest(t, handler, "GET", "/api/devices", "", http.StatusOK)
	if !strings.Contains(response.Body.String(), "大门摄像机") {
		t.Fatalf("offline device missing: %s", response.Body.String())
	}
	for _, body := range []string{`{"device_id":"123","name":"camera"}`, `{"device_id":"34020000001320000002","name":" "}`, `{"device_id":"34020000001320000002","name":"camera","password":"secret"}`} {
		apiRequest(t, handler, "POST", "/api/devices", body, http.StatusBadRequest)
	}
}

func TestChannelHTTPDerivesMediaReady(t *testing.T) {
	const deviceID = "34020000001320000001"
	const channelID = "34020000001320000002"
	const streamID = "11111111-1111-4111-8111-111111111111"
	for _, test := range []struct {
		name       string
		state      liveState
		cache      map[string]struct{}
		mediaReady bool
	}{
		{name: "no live", cache: map[string]struct{}{streamID: {}}},
		{name: "streaming without cache", state: liveStreaming},
		{name: "streaming not ready", state: liveStreaming, cache: map[string]struct{}{}},
		{name: "streaming ready", state: liveStreaming, cache: map[string]struct{}{streamID: {}}, mediaReady: true},
		{name: "other generation ready", state: liveStreaming, cache: map[string]struct{}{"old-stream": {}}},
		{name: "preparing with stale ready", state: livePreparing, cache: map[string]struct{}{streamID: {}}},
		{name: "inviting with stale ready", state: liveInviting, cache: map[string]struct{}{streamID: {}}},
		{name: "stopping with stale ready", state: liveStopping, cache: map[string]struct{}{streamID: {}}},
		{name: "cleanup with stale ready", state: liveCleanupPending, cache: map[string]struct{}{streamID: {}}},
	} {
		t.Run(test.name, func(t *testing.T) {
			s := testInfrastructure(t)
			if err := s.live.sip.deviceStore.create(t.Context(), gbDevice{deviceID: deviceID, name: "test camera"}); err != nil {
				t.Fatal(err)
			}
			s.live.sip.channels.mu.Lock()
			s.live.sip.channels.channels[deviceID] = map[string]channel{
				channelID: {deviceID: deviceID, id: channelID, status: "ON"},
			}
			s.live.sip.channels.mu.Unlock()
			if test.state != "" {
				session := testStreamingLive(s, channelID, streamID)
				s.live.mu.Lock()
				session.state = test.state
				s.live.mu.Unlock()
			}
			if test.cache != nil {
				s.readyStreams.Store(&test.cache)
			}
			response := apiRequest(t, s.handler(), "GET", "/api/devices/"+deviceID+"/channels", "", http.StatusOK)
			var body struct {
				Channels []map[string]any `json:"channels"`
			}
			if err := json.Unmarshal(response.Body.Bytes(), &body); err != nil {
				t.Fatal(err)
			}
			if len(body.Channels) != 1 {
				t.Fatalf("channels: %s", response.Body.String())
			}
			live, exists := body.Channels[0]["live"]
			if test.state == "" {
				if exists {
					t.Fatalf("channel without live includes live: %s", response.Body.String())
				}
				return
			}
			value, ok := live.(map[string]any)
			state := test.state
			if state == liveCleanupPending {
				state = liveStopping
			}
			if !ok || value["live_id"] != streamID || value["state"] != string(state) || value["media_ready"] != test.mediaReady {
				t.Fatalf("live readiness: %s, want media_ready=%t", response.Body.String(), test.mediaReady)
			}
		})
	}
}
