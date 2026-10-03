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
