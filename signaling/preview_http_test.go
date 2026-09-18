package main

import (
	"encoding/json"
	"io"
	"log/slog"
	"net/http"
	"testing"
	"time"

	"github.com/google/uuid"
)

func TestPreviewHTTPAllocatesRTSPSourceRuntime(t *testing.T) {
	registry := newMediaServerRegistry()
	registration := testMediaServerRegistration("media-1", "instance-a", "2001:db8::10")
	registration.ControlURL = "http://[2001:db8::10]:8080"
	if err := registry.register(registration, time.Now()); err != nil {
		t.Fatalf("register() error = %v", err)
	}
	server := newTestInfrastructureServer(t, testConfig(), registry, slog.New(slog.NewTextHandler(io.Discard, nil)))
	source := rtspSource{
		sourceID: uuid.NewString(), streamName: "/live/camera ?#%/", url: "rtsp://camera.example/live",
		desiredState: sourceDesiredRunning,
	}
	if err := server.sources.create(t.Context(), source); err != nil {
		t.Fatalf("create source error = %v", err)
	}
	inputStreamID := uuid.NewString()
	instance, ok := registry.selectOnline()
	if !ok {
		t.Fatal("media server is not online")
	}
	server.rtspPulls[source.sourceID] = rtspPullRuntime{
		sourceID: source.sourceID, streamID: inputStreamID, streamName: source.streamName, server: instance,
	}

	first := requestPreview(t, server, `{"source_id":"`+source.sourceID+`"}`)
	if first.StreamID == inputStreamID || !validUUIDv4(first.StreamID) {
		t.Fatalf("preview stream ID = %q, input stream ID = %q", first.StreamID, inputStreamID)
	}
	const expectedURL = "http://[2001:db8::10]:8080/play/whep/%2Flive%2Fcamera%20%3F%23%25%2F"
	if first.WHEPURL != expectedURL {
		t.Fatalf("WHEP URL = %q, want %q", first.WHEPURL, expectedURL)
	}
	allocation, ok := storedStreamAllocation(server.allocations, first.StreamID)
	if !ok || allocation.operation != streamOperationPlay || allocation.protocol != "whep" || allocation.streamName != source.streamName ||
		allocation.serverID != registration.ServerID || allocation.instanceID != registration.InstanceID {
		t.Fatalf("preview allocation = %+v, ok = %v", allocation, ok)
	}
	second := requestPreview(t, server, `{"source_id":"`+source.sourceID+`"}`)
	if second.StreamID == first.StreamID {
		t.Fatalf("successive previews reused stream ID %q", first.StreamID)
	}
}

func TestPreviewHTTPAllocatesGBLiveRuntime(t *testing.T) {
	registry := newMediaServerRegistry()
	registration := testMediaServerRegistration("media-1", "instance-a", "127.0.0.1")
	if err := registry.register(registration, time.Now()); err != nil {
		t.Fatalf("register() error = %v", err)
	}
	instance, ok := registry.selectOnline()
	if !ok {
		t.Fatal("media server is not online")
	}
	server := newTestInfrastructureServer(t, testConfig(), registry, slog.New(slog.NewTextHandler(io.Discard, nil)))
	inputStreamID := uuid.NewString()
	server.live = &liveService{sessions: map[liveKey]*liveSession{
		{deviceID: testDeviceID, channelID: testChannelID}: {
			key: liveKey{deviceID: testDeviceID, channelID: testChannelID}, streamID: inputStreamID,
			streamName: "gb/" + testDeviceID + "/" + testChannelID, server: instance, state: liveStreaming,
		},
	}}

	preview := requestPreview(t, server,
		`{"device_id":"`+testDeviceID+`","channel_id":"`+testChannelID+`"}`)
	if preview.StreamID == inputStreamID || !validUUIDv4(preview.StreamID) {
		t.Fatalf("preview stream ID = %q, input stream ID = %q", preview.StreamID, inputStreamID)
	}
	want := "http://127.0.0.1:8080/play/whep/gb%2F" + testDeviceID + "%2F" + testChannelID
	if preview.WHEPURL != want {
		t.Fatalf("WHEP URL = %q, want %q", preview.WHEPURL, want)
	}
}

func TestPreviewHTTPRejectsInvalidOrUnavailableTargets(t *testing.T) {
	registry := newMediaServerRegistry()
	server := newTestInfrastructureServer(t, testConfig(), registry, slog.New(slog.NewTextHandler(io.Discard, nil)))
	source := rtspSource{
		sourceID: uuid.NewString(), streamName: "live/camera", url: "rtsp://camera.example/live",
		desiredState: sourceDesiredStopped,
	}
	if err := server.sources.create(t.Context(), source); err != nil {
		t.Fatalf("create source error = %v", err)
	}
	server.live = &liveService{sessions: make(map[liveKey]*liveSession)}

	for name, body := range map[string]string{
		"empty":              `{}`,
		"invalid source":     `{"source_id":"invalid"}`,
		"null source":        `{"source_id":null,"device_id":"` + testDeviceID + `","channel_id":"` + testChannelID + `"}`,
		"empty source":       `{"source_id":"","device_id":"` + testDeviceID + `","channel_id":"` + testChannelID + `"}`,
		"partial GB":         `{"device_id":"` + testDeviceID + `"}`,
		"null GB field":      `{"device_id":"` + testDeviceID + `","channel_id":null}`,
		"invalid GB":         `{"device_id":"bad","channel_id":"` + testChannelID + `"}`,
		"ambiguous selector": `{"source_id":"` + source.sourceID + `","device_id":"` + testDeviceID + `","channel_id":"` + testChannelID + `"}`,
		"unknown field":      `{"source_id":"` + source.sourceID + `","server_id":"media-1"}`,
		"extra JSON":         `{"source_id":"` + source.sourceID + `"}{}`,
	} {
		t.Run(name, func(t *testing.T) {
			response := sourceRequest(t, server.handler(), http.MethodPost, "/api/preview/start", body, "application/json")
			if response.Code != http.StatusBadRequest {
				t.Fatalf("status/body = %d %s", response.Code, response.Body.String())
			}
		})
	}

	response := sourceRequest(t, server.handler(), http.MethodPost, "/api/preview/start",
		`{"source_id":"`+uuid.NewString()+`"}`, "application/json")
	if response.Code != http.StatusNotFound {
		t.Fatalf("unknown source status/body = %d %s", response.Code, response.Body.String())
	}
	response = sourceRequest(t, server.handler(), http.MethodPost, "/api/preview/start",
		`{"source_id":"`+source.sourceID+`"}`, "application/json")
	if response.Code != http.StatusConflict {
		t.Fatalf("stopped source status/body = %d %s", response.Code, response.Body.String())
	}
	response = sourceRequest(t, server.handler(), http.MethodPost, "/api/preview/start",
		`{"device_id":"`+testDeviceID+`","channel_id":"`+testChannelID+`"}`, "application/json")
	if response.Code != http.StatusConflict {
		t.Fatalf("stopped channel status/body = %d %s", response.Code, response.Body.String())
	}

	server.rtspPulls[source.sourceID] = rtspPullRuntime{
		sourceID: source.sourceID, streamID: uuid.NewString(), streamName: source.streamName,
		server: mediaServerInstance{serverID: "offline", instanceID: "instance-a", mediaIP: "127.0.0.1", httpPort: 8080},
	}
	response = sourceRequest(t, server.handler(), http.MethodPost, "/api/preview/start",
		`{"source_id":"`+source.sourceID+`"}`, "application/json")
	if response.Code != http.StatusServiceUnavailable {
		t.Fatalf("offline source status/body = %d %s", response.Code, response.Body.String())
	}

	registration := testMediaServerRegistration("offline", "instance-b", "127.0.0.2")
	if err := registry.register(registration, time.Now()); err != nil {
		t.Fatalf("register replacement error = %v", err)
	}
	response = sourceRequest(t, server.handler(), http.MethodPost, "/api/preview/start",
		`{"source_id":"`+source.sourceID+`"}`, "application/json")
	if response.Code != http.StatusServiceUnavailable {
		t.Fatalf("replacement fallback status/body = %d %s", response.Code, response.Body.String())
	}

	replacement, ok := registry.selectOnline()
	if !ok {
		t.Fatal("replacement media server is not online")
	}
	server.live.sessions[liveKey{deviceID: testDeviceID, channelID: testChannelID}] = &liveSession{
		key: liveKey{deviceID: testDeviceID, channelID: testChannelID}, streamID: uuid.NewString(),
		streamName: "gb/" + testDeviceID + "/" + testChannelID, server: replacement, state: liveStopping,
	}
	response = sourceRequest(t, server.handler(), http.MethodPost, "/api/preview/start",
		`{"device_id":"`+testDeviceID+`","channel_id":"`+testChannelID+`"}`, "application/json")
	if response.Code != http.StatusConflict {
		t.Fatalf("stopping channel status/body = %d %s", response.Code, response.Body.String())
	}
	server.live.sessions[liveKey{deviceID: testDeviceID, channelID: testChannelID}].state = liveStreaming
	server.live.sessions[liveKey{deviceID: testDeviceID, channelID: testChannelID}].server = mediaServerInstance{
		serverID: "offline", instanceID: "instance-a", mediaIP: "127.0.0.1", httpPort: 8080,
	}
	response = sourceRequest(t, server.handler(), http.MethodPost, "/api/preview/start",
		`{"device_id":"`+testDeviceID+`","channel_id":"`+testChannelID+`"}`, "application/json")
	if response.Code != http.StatusServiceUnavailable {
		t.Fatalf("stale GB instance status/body = %d %s", response.Code, response.Body.String())
	}

	response = sourceRequest(t, server.handler(), http.MethodPost, "/api/preview/start",
		`{"source_id":"`+source.sourceID+`"}`, "text/plain")
	if response.Code != http.StatusBadRequest {
		t.Fatalf("wrong content type status/body = %d %s", response.Code, response.Body.String())
	}
	response = sourceRequest(t, server.handler(), http.MethodGet, "/api/preview/start", "", "")
	if response.Code != http.StatusMethodNotAllowed {
		t.Fatalf("wrong method status/body = %d %s", response.Code, response.Body.String())
	}
}

type previewTestResponse struct {
	StreamID string `json:"stream_id"`
	WHEPURL  string `json:"whep_url"`
}

func requestPreview(t *testing.T, server *infrastructureServer, body string) previewTestResponse {
	t.Helper()
	response := sourceRequest(t, server.handler(), http.MethodPost, "/api/preview/start", body, "application/json")
	if response.Code != http.StatusCreated {
		t.Fatalf("preview status/body = %d %s", response.Code, response.Body.String())
	}
	var payload previewTestResponse
	if err := json.Unmarshal(response.Body.Bytes(), &payload); err != nil {
		t.Fatalf("decode preview response error = %v", err)
	}
	return payload
}
