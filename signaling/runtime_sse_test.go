package main

import (
	"bufio"
	"context"
	"encoding/json"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"
)

func TestObservedRuntimeNotifierFollowsTransitions(t *testing.T) {
	runtimes := newObservedRuntimeRegistry()
	changes := make(chan observedRuntime, 4)
	runtimes.setOnChange(func(runtime observedRuntime) { changes <- runtime })
	starting := testObservedRuntime("00000000-0000-4000-8000-000000000011", "starting")
	if _, err := runtimes.apply(starting); err != nil {
		t.Fatalf("apply(starting) error = %v", err)
	}
	if _, err := runtimes.apply(starting); err != nil {
		t.Fatalf("apply(duplicate) error = %v", err)
	}
	streaming := starting
	streaming.State = "streaming"
	streaming.Stage = "streaming"
	if _, err := runtimes.apply(streaming); err != nil {
		t.Fatalf("apply(streaming) error = %v", err)
	}
	server := mediaServerInstance{serverID: starting.ServerID, instanceID: starting.InstanceID}
	if _, err := runtimes.acknowledgeSourceStopped(
		server, starting.StreamID, starting.StreamName, starting.SourceID, starting.Protocol); err != nil {
		t.Fatalf("acknowledgeSourceStopped() error = %v", err)
	}

	wantStates := []string{"starting", "streaming", "stopped"}
	for _, want := range wantStates {
		select {
		case runtime := <-changes:
			if runtime.State != want {
				t.Fatalf("notified state = %q, want %q", runtime.State, want)
			}
		case <-time.After(time.Second):
			t.Fatalf("missing %q notification", want)
		}
	}
	select {
	case runtime := <-changes:
		t.Fatalf("unexpected notification = %+v", runtime)
	default:
	}
}

func TestObservedRuntimeNotifierIncludesMediaServerOffline(t *testing.T) {
	runtimes := newObservedRuntimeRegistry()
	changes := make(chan observedRuntime, 3)
	runtimes.setOnChange(func(runtime observedRuntime) { changes <- runtime })
	active := testObservedRuntime("00000000-0000-4000-8000-000000000012", "streaming")
	if _, err := runtimes.apply(active); err != nil {
		t.Fatalf("apply(active) error = %v", err)
	}
	<-changes
	runtimes.mediaServerOffline(active.ServerID, active.InstanceID)
	terminal := <-changes
	if terminal.StreamID != active.StreamID || terminal.Kind != active.Kind || terminal.State != "stopped" ||
		terminal.EndReason != "runtime_error" || terminal.Error != "media_server_offline" {
		t.Fatalf("offline notification = %+v", terminal)
	}
	runtimes.mediaServerOffline(active.ServerID, active.InstanceID)
	select {
	case runtime := <-changes:
		t.Fatalf("duplicate offline notification = %+v", runtime)
	default:
	}
}

func TestRuntimeEventHubFanoutAndSlowSubscriber(t *testing.T) {
	hub := newRuntimeEventHub()
	left, ok := hub.subscribe()
	if !ok {
		t.Fatal("left subscriber rejected")
	}
	right, ok := hub.subscribe()
	if !ok {
		t.Fatal("right subscriber rejected")
	}
	first := testObservedRuntime("00000000-0000-4000-8000-000000000021", "starting")
	second := first
	second.State = "streaming"
	second.Stage = "streaming"
	for _, event := range []observedRuntime{first, second} {
		hub.publish(event)
		if received := receiveRuntime(t, left); received != event {
			t.Fatalf("left event = %+v, want %+v", received, event)
		}
		if received := receiveRuntime(t, right); received != event {
			t.Fatalf("right event = %+v, want %+v", received, event)
		}
	}

	slow, ok := hub.subscribe()
	if !ok {
		t.Fatal("slow subscriber rejected")
	}
	hub.publish(first)
	_ = receiveRuntime(t, left)
	_ = receiveRuntime(t, right)
	done := make(chan struct{})
	go func() {
		hub.publish(second)
		close(done)
	}()
	select {
	case <-done:
	case <-time.After(time.Second):
		t.Fatal("slow subscriber blocked publish")
	}
	_ = receiveRuntime(t, left)
	_ = receiveRuntime(t, right)
	if received := receiveRuntime(t, slow); received != first {
		t.Fatalf("slow subscriber retained = %+v, want %+v", received, first)
	}
	if _, open := <-slow; open {
		t.Fatal("slow subscriber remained open")
	}

	hub.close()
	hub.close()
	if _, open := <-left; open {
		t.Fatal("left subscriber remained open")
	}
	if _, open := <-right; open {
		t.Fatal("right subscriber remained open")
	}
	if _, ok := hub.subscribe(); ok {
		t.Fatal("closed hub accepted subscriber")
	}
}

func TestRuntimeSSEDeliversAppliedStateAndCloses(t *testing.T) {
	registry := newMediaServerRegistry()
	registration := testMediaServerRegistration("media-1", "instance-a", "127.0.0.1")
	if err := registry.register(registration, time.Now()); err != nil {
		t.Fatalf("register() error = %v", err)
	}
	server := newTestInfrastructureServer(t, testConfig(), registry, slog.New(slog.NewTextHandler(io.Discard, nil)))
	httpServer := httptest.NewServer(server.handler())
	t.Cleanup(func() {
		server.runtimeEvents.close()
		httpServer.Close()
	})
	ctx, cancel := context.WithTimeout(t.Context(), 3*time.Second)
	defer cancel()
	request, err := http.NewRequestWithContext(ctx, http.MethodGet, httpServer.URL+"/api/events", nil)
	if err != nil {
		t.Fatalf("NewRequestWithContext() error = %v", err)
	}
	response, err := httpServer.Client().Do(request)
	if err != nil {
		t.Fatalf("SSE request error = %v", err)
	}
	defer response.Body.Close()
	if response.StatusCode != http.StatusOK || response.Header.Get("Content-Type") != "text/event-stream" ||
		response.Header.Get("Cache-Control") != "no-cache" {
		t.Fatalf("SSE response = %d, headers = %v", response.StatusCode, response.Header)
	}
	reader := bufio.NewReader(response.Body)
	if line := readSSELine(t, reader); line != ": connected\n" {
		t.Fatalf("SSE greeting = %q", line)
	}
	if line := readSSELine(t, reader); line != "\n" {
		t.Fatalf("SSE greeting separator = %q", line)
	}

	event := testObservedRuntime("00000000-0000-4000-8000-000000000031", "starting")
	event.Kind = "publisher"
	event.Protocol = "rtmp"
	eventResponse := postJSON(t, httpServer.Client(), httpServer.URL+"/internal/runtime-events", runtimeEventBatchValue(event))
	if eventResponse.StatusCode != http.StatusNoContent {
		t.Fatalf("runtime event status/body = %d %s", eventResponse.StatusCode, readBody(t, eventResponse))
	}
	eventResponse.Body.Close()
	if line := readSSELine(t, reader); line != "event: runtime\n" {
		t.Fatalf("SSE event line = %q", line)
	}
	dataLine := readSSELine(t, reader)
	if !strings.HasPrefix(dataLine, "data: ") {
		t.Fatalf("SSE data line = %q", dataLine)
	}
	var delivered observedRuntime
	if err := json.Unmarshal([]byte(strings.TrimSuffix(strings.TrimPrefix(dataLine, "data: "), "\n")), &delivered); err != nil {
		t.Fatalf("decode SSE runtime error = %v", err)
	}
	if delivered != event {
		t.Fatalf("SSE runtime = %+v, want %+v", delivered, event)
	}
	if line := readSSELine(t, reader); line != "\n" {
		t.Fatalf("SSE event separator = %q", line)
	}

	server.runtimeEvents.close()
	if _, err := reader.ReadString('\n'); err == nil {
		t.Fatal("SSE connection remained open after hub close")
	}
}

func receiveRuntime(t *testing.T, events <-chan observedRuntime) observedRuntime {
	t.Helper()
	select {
	case event := <-events:
		return event
	case <-time.After(time.Second):
		t.Fatal("runtime event timeout")
		return observedRuntime{}
	}
}

func readSSELine(t *testing.T, reader *bufio.Reader) string {
	t.Helper()
	type result struct {
		line string
		err  error
	}
	done := make(chan result, 1)
	go func() {
		line, err := reader.ReadString('\n')
		done <- result{line: line, err: err}
	}()
	select {
	case value := <-done:
		if value.err != nil {
			t.Fatalf("read SSE line error = %v", value.err)
		}
		return value.line
	case <-time.After(time.Second):
		t.Fatal("SSE line timeout")
		return ""
	}
}
