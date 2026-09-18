package main

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"
)

func runtimeEventBatch(events ...string) string {
	return runtimeEventBatchFor("media-1", "instance-a", events...)
}

func runtimeEventBatchFor(serverID, instanceID string, events ...string) string {
	rawEvents := make([]json.RawMessage, len(events))
	for index, event := range events {
		rawEvents[index] = json.RawMessage(event)
	}
	body, err := json.Marshal(struct {
		ServerID   string            `json:"server_id"`
		InstanceID string            `json:"instance_id"`
		Events     []json.RawMessage `json:"events"`
	}{ServerID: serverID, InstanceID: instanceID, Events: rawEvents})
	if err != nil {
		panic(err)
	}
	return string(body)
}

func runtimeEventBatchValue(event observedRuntime) map[string]any {
	body, err := json.Marshal(event)
	if err != nil {
		panic(err)
	}
	var value map[string]any
	if err := json.Unmarshal(body, &value); err != nil {
		panic(err)
	}
	delete(value, "server_id")
	delete(value, "instance_id")
	return map[string]any{
		"server_id": event.ServerID, "instance_id": event.InstanceID,
		"events": []map[string]any{value},
	}
}

func TestRuntimeEventHTTPAndSnapshot(t *testing.T) {
	server, registration := newRuntimeHTTPTestServer(t)
	sourceID := "10000000-0000-4000-8000-000000000001"
	streamID := "00000000-0000-4000-8000-000000000001"
	server.runtimes.bindSource(sourceID, streamID)
	event := `{"kind":"source","stream_id":"` + streamID + `","stream_name":"live/camera",` +
		`"source_id":"` + sourceID + `","protocol":"rtsp",` +
		`"state":"starting","stage":"resolving"}`
	response := sourceRequest(t, server.handler(), http.MethodPost, "/internal/runtime-events", runtimeEventBatch(event), "application/json")
	if response.Code != http.StatusNoContent || response.Body.Len() != 0 {
		t.Fatalf("event response = %d %q", response.Code, response.Body.String())
	}
	response = sourceRequest(t, server.handler(), http.MethodGet, "/api/runtimes", "", "")
	if response.Code != http.StatusOK {
		t.Fatalf("runtime list status/body = %d %s", response.Code, response.Body.String())
	}
	if body := response.Body.String(); strings.Contains(body, `"type"`) || strings.Contains(body, `"direction"`) {
		t.Fatalf("runtime snapshot contains legacy fields: %s", body)
	}
	var snapshot struct {
		Runtimes []observedRuntime `json:"runtimes"`
	}
	if err := json.Unmarshal(response.Body.Bytes(), &snapshot); err != nil || len(snapshot.Runtimes) != 1 {
		t.Fatalf("runtime snapshot = %+v, %v", snapshot, err)
	}
	if runtime := snapshot.Runtimes[0]; runtime.Kind != "source" || runtime.StreamName != "live/camera" || runtime.State != "starting" ||
		runtime.ServerID != registration.ServerID || runtime.SourceID == "" {
		t.Fatalf("runtime = %+v", runtime)
	}
}

func TestRuntimeEventHTTPAcceptsAdditionalProtocols(t *testing.T) {
	server, _ := newRuntimeHTTPTestServer(t)
	for index, eventType := range []struct {
		kind     string
		protocol string
	}{{kind: "output", protocol: "http-flv"}, {kind: "output", protocol: "hls"}, {kind: "publisher", protocol: "whip"}} {
		event := fmt.Sprintf(
			`{"kind":%q,"stream_id":"00000000-0000-4000-8000-%012d","stream_name":"live/camera","protocol":%q,"state":"starting","stage":"play"}`,
			eventType.kind, index+1, eventType.protocol,
		)
		response := sourceRequest(t, server.handler(), http.MethodPost, "/internal/runtime-events", runtimeEventBatch(event), "application/json")
		if response.Code != http.StatusNoContent {
			t.Fatalf("%s event response = %d %s", eventType.protocol, response.Code, response.Body.String())
		}
	}
}

func TestRuntimeEventHTTPRejectsInvalidStaleAndConflictingEvents(t *testing.T) {
	server, _ := newRuntimeHTTPTestServer(t)
	valid := `{"kind":"publisher","stream_id":"00000000-0000-4000-8000-000000000001","stream_name":"live/camera",` +
		`"protocol":"rtmp","state":"starting","stage":"publish"}`
	if response := sourceRequest(t, server.handler(), http.MethodPost, "/internal/runtime-events", runtimeEventBatch(valid), "application/json"); response.Code != http.StatusNoContent {
		t.Fatalf("valid event response = %d %s", response.Code, response.Body.String())
	}
	tests := []struct {
		name string
		body string
		want int
	}{
		{name: "unknown field", body: runtimeEventBatch(valid[:len(valid)-1] + `,"extra":true}`), want: http.StatusBadRequest},
		{name: "missing stream id", body: runtimeEventBatch(`{"kind":"source","stream_name":"live/camera","source_id":"10000000-0000-4000-8000-000000000001","protocol":"rtsp","state":"starting"}`), want: http.StatusBadRequest},
		{name: "invalid stream id", body: runtimeEventBatch(`{"kind":"source","stream_id":"invalid","stream_name":"live/camera","source_id":"10000000-0000-4000-8000-000000000001","protocol":"rtsp","state":"starting"}`), want: http.StatusBadRequest},
		{name: "missing rtsp source id", body: runtimeEventBatch(`{"kind":"source","stream_id":"20000000-0000-4000-8000-000000000001","stream_name":"live/camera","protocol":"rtsp","state":"starting"}`), want: http.StatusBadRequest},
		{name: "invalid kind", body: runtimeEventBatch(`{"kind":"receiver","stream_id":"20000000-0000-4000-8000-000000000001","stream_name":"live/camera","protocol":"rtsp","state":"streaming"}`), want: http.StatusBadRequest},
		{name: "invalid protocol", body: runtimeEventBatch(`{"kind":"source","stream_id":"20000000-0000-4000-8000-000000000001","stream_name":"live/camera","protocol":"dash","state":"starting"}`), want: http.StatusBadRequest},
		{name: "invalid state", body: runtimeEventBatch(`{"kind":"source","stream_id":"20000000-0000-4000-8000-000000000001","stream_name":"live/camera","source_id":"10000000-0000-4000-8000-000000000001","protocol":"rtsp","state":"failed"}`), want: http.StatusBadRequest},
		{name: "removed end reason", body: runtimeEventBatch(`{"kind":"source","stream_id":"20000000-0000-4000-8000-000000000001","stream_name":"live/camera","source_id":"10000000-0000-4000-8000-000000000001","protocol":"rtsp","state":"starting","end_reason":"remote"}`), want: http.StatusBadRequest},
		{name: "active error", body: runtimeEventBatch(`{"kind":"source","stream_id":"20000000-0000-4000-8000-000000000001","stream_name":"live/camera","source_id":"10000000-0000-4000-8000-000000000001","protocol":"rtsp","state":"starting","error":"failed"}`), want: http.StatusBadRequest},
		{name: "legacy type", body: runtimeEventBatch(`{"type":"source_started","kind":"source","stream_id":"20000000-0000-4000-8000-000000000001","stream_name":"live/camera","protocol":"rtsp","state":"starting"}`), want: http.StatusBadRequest},
		{name: "legacy direction", body: runtimeEventBatch(`{"kind":"source","stream_id":"20000000-0000-4000-8000-000000000001","stream_name":"live/camera","direction":"input","protocol":"rtsp","state":"starting"}`), want: http.StatusBadRequest},
		{name: "event server identity", body: runtimeEventBatch(`{"kind":"publisher","server_id":"media-1","instance_id":"instance-a","stream_id":"20000000-0000-4000-8000-000000000001","stream_name":"live/camera","protocol":"rtmp","state":"starting"}`), want: http.StatusBadRequest},
		{name: "empty batch", body: runtimeEventBatch(), want: http.StatusBadRequest},
		{name: "missing server identity", body: runtimeEventBatchFor("", "instance-a", valid), want: http.StatusBadRequest},
		{name: "old single event", body: valid, want: http.StatusBadRequest},
		{name: "stale instance", body: runtimeEventBatchFor("media-1", "instance-old", `{"kind":"source","stream_id":"20000000-0000-4000-8000-000000000001","stream_name":"live/camera","source_id":"10000000-0000-4000-8000-000000000001","protocol":"rtsp","state":"starting"}`), want: http.StatusGone},
		{name: "identity mutation", body: runtimeEventBatch(`{"kind":"publisher","stream_id":"00000000-0000-4000-8000-000000000001","stream_name":"live/other","protocol":"rtmp","state":"streaming","stage":"streaming"}`), want: http.StatusConflict},
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

func TestRuntimeEventBatchValidatesBeforeApplyingAndPreservesOrder(t *testing.T) {
	server, _ := newRuntimeHTTPTestServer(t)
	streamID := "40000000-0000-4000-8000-000000000001"
	starting := `{"kind":"publisher","stream_id":"` + streamID + `","stream_name":"live/camera","protocol":"rtmp","state":"starting","stage":"publish"}`
	invalid := `{"kind":"publisher","stream_id":"invalid","stream_name":"live/camera","protocol":"rtmp","state":"streaming","stage":"streaming"}`
	response := sourceRequest(t, server.handler(), http.MethodPost, "/internal/runtime-events", runtimeEventBatch(starting, invalid), "application/json")
	if response.Code != http.StatusBadRequest || len(server.runtimes.snapshot()) != 0 {
		t.Fatalf("invalid batch status/runtimes = %d/%+v", response.Code, server.runtimes.snapshot())
	}
	regressionStreamID := "40000000-0000-4000-8000-000000000002"
	streamingRegression := `{"kind":"publisher","stream_id":"` + regressionStreamID + `","stream_name":"live/regression","protocol":"rtmp","state":"streaming","stage":"streaming"}`
	startingRegression := `{"kind":"publisher","stream_id":"` + regressionStreamID + `","stream_name":"live/regression","protocol":"rtmp","state":"starting","stage":"publish"}`
	response = sourceRequest(t, server.handler(), http.MethodPost, "/internal/runtime-events", runtimeEventBatch(streamingRegression, startingRegression), "application/json")
	snapshot := server.runtimes.snapshot()
	if response.Code != http.StatusNoContent || len(snapshot) != 1 || snapshot[0].State != "starting" {
		t.Fatalf("out-of-order batch status/runtimes = %d/%+v", response.Code, snapshot)
	}

	existingStreamID := "40000000-0000-4000-8000-000000000003"
	existing := `{"kind":"publisher","stream_id":"` + existingStreamID + `","stream_name":"live/existing","protocol":"rtmp","state":"starting","stage":"publish"}`
	response = sourceRequest(t, server.handler(), http.MethodPost, "/internal/runtime-events", runtimeEventBatch(existing), "application/json")
	if response.Code != http.StatusNoContent {
		t.Fatalf("existing event response = %d %s", response.Code, response.Body.String())
	}
	changes := 0
	server.runtimes.setOnChange(func(observedRuntime) { changes++ })
	newStarting := `{"kind":"publisher","stream_id":"40000000-0000-4000-8000-000000000004","stream_name":"live/new","protocol":"rtmp","state":"starting","stage":"publish"}`
	conflicting := `{"kind":"publisher","stream_id":"` + existingStreamID + `","stream_name":"live/other","protocol":"rtmp","state":"streaming","stage":"streaming"}`
	response = sourceRequest(t, server.handler(), http.MethodPost, "/internal/runtime-events", runtimeEventBatch(newStarting, conflicting), "application/json")
	snapshot = server.runtimes.snapshot()
	if response.Code != http.StatusConflict || len(snapshot) != 2 || snapshot[1].StreamID != existingStreamID || changes != 0 {
		t.Fatalf("conflicting batch status/runtimes/changes = %d/%+v/%d", response.Code, snapshot, changes)
	}

	var states []string
	server.runtimes.setOnChange(func(event observedRuntime) { states = append(states, event.State) })
	streaming := `{"kind":"publisher","stream_id":"` + streamID + `","stream_name":"live/camera","protocol":"rtmp","state":"streaming","stage":"streaming"}`
	stopped := `{"kind":"publisher","stream_id":"` + streamID + `","stream_name":"live/camera","protocol":"rtmp","state":"stopped"}`
	body := runtimeEventBatch(starting, streaming, stopped)
	response = sourceRequest(t, server.handler(), http.MethodPost, "/internal/runtime-events", body, "application/json")
	if response.Code != http.StatusNoContent || strings.Join(states, ",") != "starting,streaming,stopped" {
		t.Fatalf("ordered batch status/states = %d/%v", response.Code, states)
	}
	response = sourceRequest(t, server.handler(), http.MethodPost, "/internal/runtime-events", body, "application/json")
	if response.Code != http.StatusNoContent || len(states) != 3 {
		t.Fatalf("replayed batch status/states = %d/%v", response.Code, states)
	}
}

func TestRuntimeEventBatchAcceptsFullReporterCapacity(t *testing.T) {
	server, _ := newRuntimeHTTPTestServer(t)
	events := make([]string, 500)
	for index := range events {
		events[index] = fmt.Sprintf(
			`{"kind":"publisher","stream_id":"00000000-0000-4000-8000-%012d","stream_name":"live/camera-%d","protocol":"rtmp","state":"starting","stage":"publish"}`,
			index, index,
		)
	}
	response := sourceRequest(t, server.handler(), http.MethodPost, "/internal/runtime-events", runtimeEventBatch(events...), "application/json")
	if response.Code != http.StatusNoContent || len(server.runtimes.snapshot()) != len(events) {
		t.Fatalf("full batch status/runtime count = %d/%d", response.Code, len(server.runtimes.snapshot()))
	}
}

func TestRuntimeEventHTTPDoesNotRemoveReplacementPull(t *testing.T) {
	server, registration := newRuntimeHTTPTestServer(t)
	sourceID := "10000000-0000-4000-8000-000000000001"
	oldStreamID := "20000000-0000-4000-8000-000000000001"
	oldStarting := observedRuntime{
		Kind: "source", ServerID: registration.ServerID, InstanceID: registration.InstanceID,
		StreamID: oldStreamID, StreamName: "live/camera", SourceID: sourceID,
		Protocol: "rtsp", State: "starting", Stage: "resolving",
	}
	server.runtimes.bindSource(sourceID, oldStreamID)
	if _, err := server.runtimes.apply(oldStarting); err != nil {
		t.Fatalf("apply old runtime error = %v", err)
	}
	replacement := rtspPullRuntime{
		sourceID: sourceID, streamName: "live/camera",
		server:   mediaServerInstance{serverID: registration.ServerID, instanceID: registration.InstanceID},
		streamID: "30000000-0000-4000-8000-000000000001",
	}
	server.rtspPulls[sourceID] = replacement
	server.runtimes.bindSource(sourceID, replacement.streamID)
	oldStop := `{"kind":"source","stream_id":"` + oldStreamID + `","stream_name":"live/camera",` +
		`"source_id":"` + sourceID + `","protocol":"rtsp","state":"stopped"}`
	response := sourceRequest(t, server.handler(), http.MethodPost, "/internal/runtime-events", runtimeEventBatch(oldStop), "application/json")
	if response.Code != http.StatusNoContent {
		t.Fatalf("stop response = %d %s", response.Code, response.Body.String())
	}
	if current, ok := server.rtspSourceRuntime(sourceID); !ok || current.streamID != replacement.streamID {
		t.Fatalf("replacement runtime = %+v, %v", current, ok)
	}
}

func TestRuntimeEventHTTPRemovesOnlyMatchingGBLiveGeneration(t *testing.T) {
	server, registration := newRuntimeHTTPTestServer(t)
	platform := newTestSIPServer(t, "127.0.0.1:0")
	allocator, err := newSSRCAllocator(platform.cfg.sipDomain)
	if err != nil {
		t.Fatalf("newSSRCAllocator() error = %v", err)
	}
	live := newLiveService(platform, server.registry, server.media, allocator,
		slog.New(slog.NewTextHandler(io.Discard, nil)))
	server.live = live
	ssrc, err := allocator.acquire()
	if err != nil {
		t.Fatalf("acquire SSRC error = %v", err)
	}
	_, cancel := context.WithCancel(context.Background())
	t.Cleanup(cancel)
	replacementID := "30000000-0000-4000-8000-000000000001"
	session := &liveSession{
		key:      liveKey{deviceID: testDeviceID, channelID: testChannelID},
		streamID: replacementID, streamName: "gb/" + testDeviceID + "/" + testChannelID,
		server: mediaServerInstance{serverID: registration.ServerID, instanceID: registration.InstanceID},
		ssrc:   ssrc, state: liveStreaming, cancel: cancel, established: make(chan struct{}), done: make(chan struct{}),
	}
	live.sessions[session.key] = session
	outputStop := `{"kind":"output","stream_id":"10000000-0000-4000-8000-000000000002","stream_name":"` + session.streamName + `",` +
		`"protocol":"gb28181","state":"stopped"}`
	if response := sourceRequest(t, server.handler(), http.MethodPost, "/internal/runtime-events", runtimeEventBatch(outputStop), "application/json"); response.Code != http.StatusNoContent || live.len() != 1 {
		t.Fatalf("output terminal status/live = %d/%d", response.Code, live.len())
	}
	postStop := func(streamID string) *httptest.ResponseRecorder {
		body := `{"kind":"source","stream_id":"` + streamID + `","stream_name":"` + session.streamName + `",` +
			`"protocol":"gb28181","state":"stopped"}`
		return sourceRequest(t, server.handler(), http.MethodPost, "/internal/runtime-events", runtimeEventBatch(body), "application/json")
	}
	if response := postStop("20000000-0000-4000-8000-000000000001"); response.Code != http.StatusNoContent || live.len() != 1 {
		t.Fatalf("stale terminal status/live = %d/%d", response.Code, live.len())
	}
	if response := postStop(replacementID); response.Code != http.StatusNoContent || live.len() != 0 || allocator.activeCount() != 0 {
		t.Fatalf("matching terminal status/live/ssrc = %d/%d/%d", response.Code, live.len(), allocator.activeCount())
	}
	select {
	case <-session.done:
	default:
		t.Fatal("matching terminal did not finish session cleanup")
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
	event := `{"kind":"source","stream_id":"00000000-0000-4000-8000-000000000001","stream_name":"live/camera",` +
		`"source_id":"10000000-0000-4000-8000-000000000001","protocol":"rtsp","state":"streaming","stage":"streaming"}`
	response := sourceRequest(t, server.handler(), http.MethodPost, "/internal/runtime-events", runtimeEventBatch(event), "application/json")
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
