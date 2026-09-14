package main

import (
	"encoding/json"
	"io"
	"log/slog"
	"net/http"
	"testing"
	"time"
)

func TestRuntimeEventHTTPAndSnapshot(t *testing.T) {
	server, registration := newRuntimeHTTPTestServer(t)
	event := `{"type":"source_started","server_id":"media-1","instance_id":"instance-a",` +
		`"stream_id":"00000000-0000-4000-8000-000000000001","stream_name":"live/camera",` +
		`"source_id":"10000000-0000-4000-8000-000000000001","direction":"input","protocol":"rtsp",` +
		`"state":"starting","stage":"resolving"}`
	response := sourceRequest(t, server.handler(), http.MethodPost, "/internal/runtime-events", event, "application/json")
	if response.Code != http.StatusNoContent || response.Body.Len() != 0 {
		t.Fatalf("event response = %d %q", response.Code, response.Body.String())
	}
	response = sourceRequest(t, server.handler(), http.MethodGet, "/api/runtimes", "", "")
	if response.Code != http.StatusOK {
		t.Fatalf("runtime list status/body = %d %s", response.Code, response.Body.String())
	}
	var snapshot struct {
		Runtimes []observedRuntime `json:"runtimes"`
	}
	if err := json.Unmarshal(response.Body.Bytes(), &snapshot); err != nil || len(snapshot.Runtimes) != 1 {
		t.Fatalf("runtime snapshot = %+v, %v", snapshot, err)
	}
	if runtime := snapshot.Runtimes[0]; runtime.StreamName != "live/camera" || runtime.State != "starting" ||
		runtime.ServerID != registration.ServerID || runtime.SourceID == "" {
		t.Fatalf("runtime = %+v", runtime)
	}
}

func TestRuntimeEventHTTPRejectsInvalidStaleAndConflictingEvents(t *testing.T) {
	server, _ := newRuntimeHTTPTestServer(t)
	valid := `{"type":"publisher_connected","server_id":"media-1","instance_id":"instance-a",` +
		`"stream_id":"00000000-0000-4000-8000-000000000001","stream_name":"live/camera",` +
		`"direction":"input","protocol":"rtmp","state":"starting","stage":"publish"}`
	if response := sourceRequest(t, server.handler(), http.MethodPost, "/internal/runtime-events", valid, "application/json"); response.Code != http.StatusNoContent {
		t.Fatalf("valid event response = %d %s", response.Code, response.Body.String())
	}
	tests := []struct {
		name string
		body string
		want int
	}{
		{name: "unknown field", body: valid[:len(valid)-1] + `,"extra":true}`, want: http.StatusBadRequest},
		{name: "missing stream id", body: `{"type":"source_started","server_id":"media-1","instance_id":"instance-a","stream_name":"live/camera","direction":"input","protocol":"rtsp","state":"starting"}`, want: http.StatusBadRequest},
		{name: "invalid transition shape", body: `{"type":"source_stopped","server_id":"media-1","instance_id":"instance-a","stream_id":"20000000-0000-4000-8000-000000000001","stream_name":"live/camera","direction":"input","protocol":"rtsp","state":"streaming"}`, want: http.StatusBadRequest},
		{name: "missing end reason", body: `{"type":"runtime_error","server_id":"media-1","instance_id":"instance-a","stream_id":"20000000-0000-4000-8000-000000000001","stream_name":"live/camera","direction":"input","protocol":"rtsp","state":"stopped"}`, want: http.StatusBadRequest},
		{name: "stale instance", body: `{"type":"source_started","server_id":"media-1","instance_id":"instance-old","stream_id":"20000000-0000-4000-8000-000000000001","stream_name":"live/camera","direction":"input","protocol":"rtsp","state":"starting"}`, want: http.StatusGone},
		{name: "identity mutation", body: `{"type":"publisher_connected","server_id":"media-1","instance_id":"instance-a","stream_id":"00000000-0000-4000-8000-000000000001","stream_name":"live/other","direction":"input","protocol":"rtmp","state":"streaming","stage":"streaming"}`, want: http.StatusConflict},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			response := sourceRequest(t, server.handler(), http.MethodPost, "/internal/runtime-events", test.body, "application/json")
			if response.Code != test.want {
				t.Fatalf("status/body = %d %s", response.Code, response.Body.String())
			}
		})
	}
}

func TestRuntimeEventHTTPDoesNotRemoveReplacementPull(t *testing.T) {
	server, registration := newRuntimeHTTPTestServer(t)
	replacement := rtspPullRuntime{
		server:   mediaServerInstance{serverID: registration.ServerID, instanceID: registration.InstanceID},
		streamID: "30000000-0000-4000-8000-000000000001",
	}
	server.rtspPulls["live/camera"] = replacement
	oldStop := `{"type":"source_stopped","server_id":"media-1","instance_id":"instance-a",` +
		`"stream_id":"20000000-0000-4000-8000-000000000001","stream_name":"live/camera",` +
		`"direction":"input","protocol":"rtsp","state":"stopped","end_reason":"remote"}`
	response := sourceRequest(t, server.handler(), http.MethodPost, "/internal/runtime-events", oldStop, "application/json")
	if response.Code != http.StatusNoContent {
		t.Fatalf("stop response = %d %s", response.Code, response.Body.String())
	}
	if current, ok := server.rtspPullRuntime("live/camera"); !ok || current.streamID != replacement.streamID {
		t.Fatalf("replacement runtime = %+v, %v", current, ok)
	}
}

func TestRuntimeEventHTTPRejectsEventsAfterMediaServerExpiry(t *testing.T) {
	server, _ := newRuntimeHTTPTestServer(t)
	now := time.Now()
	offline := server.registry.expire(now.Add(time.Hour), time.Minute)
	if len(offline) != 1 {
		t.Fatalf("expired instances = %d", len(offline))
	}
	server.runtimes.mediaServerOffline(offline[0].serverID, offline[0].instanceID)
	event := `{"type":"source_started","server_id":"media-1","instance_id":"instance-a",` +
		`"stream_id":"00000000-0000-4000-8000-000000000001","stream_name":"live/camera",` +
		`"direction":"input","protocol":"rtsp","state":"streaming","stage":"streaming"}`
	response := sourceRequest(t, server.handler(), http.MethodPost, "/internal/runtime-events", event, "application/json")
	if response.Code != http.StatusGone {
		t.Fatalf("late event response = %d %s", response.Code, response.Body.String())
	}
}

func TestMediaServerHTTPListsOnlyCurrentInstances(t *testing.T) {
	server, _ := newRuntimeHTTPTestServer(t)
	offline := server.registry.expire(time.Now().Add(time.Hour), time.Minute)
	if len(offline) != 1 {
		t.Fatalf("expired instances = %d", len(offline))
	}
	if err := server.registry.register(mediaServerRegistration{
		ServerID: "media-1", InstanceID: "instance-b", ControlURL: "http://127.0.0.1:19091",
		MediaIP: "192.0.2.20", RTMPPort: 1936, RTSPPort: 8555, HTTPPort: 8081,
	}, time.Unix(200, 0)); err != nil {
		t.Fatalf("register replacement error = %v", err)
	}
	response := sourceRequest(t, server.handler(), http.MethodGet, "/api/media-servers", "", "")
	var snapshot struct {
		MediaServers []mediaServerResponse `json:"media_servers"`
	}
	if response.Code != http.StatusOK {
		t.Fatalf("media server list response = %d %s", response.Code, response.Body.String())
	}
	if err := json.Unmarshal(response.Body.Bytes(), &snapshot); err != nil || len(snapshot.MediaServers) != 1 {
		t.Fatalf("media server snapshot = %+v, %v", snapshot, err)
	}
	current := snapshot.MediaServers[0]
	if current.InstanceID != "instance-b" || !current.Online || current.MediaIP != "192.0.2.20" ||
		current.RTMPPort != 1936 || current.RTSPPort != 8555 || current.HTTPPort != 8081 || !current.LastSeen.Equal(time.Unix(200, 0)) {
		t.Fatalf("current media server = %+v", current)
	}
}

func newRuntimeHTTPTestServer(t *testing.T) (*infrastructureServer, mediaServerRegistration) {
	t.Helper()
	registry := newMediaServerRegistry()
	registration := mediaServerRegistration{
		ServerID: "media-1", InstanceID: "instance-a", ControlURL: "http://127.0.0.1:19090",
		MediaIP: "192.0.2.10", RTMPPort: 1935, RTSPPort: 8554, HTTPPort: 8080,
	}
	if err := registry.register(registration, time.Now()); err != nil {
		t.Fatalf("register() error = %v", err)
	}
	return newTestInfrastructureServer(
		t, testConfig(), registry, slog.New(slog.NewTextHandler(io.Discard, nil))), registration
}
