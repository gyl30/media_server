package main

import (
	"encoding/json"
	"net"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync/atomic"
	"testing"
)

func TestControlClientStartAndStop(t *testing.T) {
	var starts, stops int
	server := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
		var command liveControlRequest
		if err := json.NewDecoder(request.Body).Decode(&command); err != nil {
			t.Errorf("Decode() error = %v", err)
			return
		}
		if command.DeviceID != "34020000001320000001" || command.ChannelID != "34020000001320000002" {
			t.Errorf("command = %+v", command)
		}
		writer.Header().Set("Content-Type", "application/json")
		switch request.URL.Path {
		case "/internal/live/start":
			starts++
			writer.WriteHeader(http.StatusCreated)
			_, _ = writer.Write([]byte(`{"result":"ok","stream_name":"gb/a/b","state":"streaming","ssrc":200000001,"rtp_port":40000}`))
		case "/internal/live/stop":
			stops++
			_, _ = writer.Write([]byte(`{"result":"ok"}`))
		default:
			http.NotFound(writer, request)
		}
	}))
	defer server.Close()

	client := newControlClient(server.URL)
	started, err := client.startLive(t.Context(), "34020000001320000001", "34020000001320000002")
	if err != nil {
		t.Fatalf("startLive() error = %v", err)
	}
	if started.State != "streaming" || started.SSRC != 200000001 || started.RTPPort != 40000 {
		t.Fatalf("started = %+v", started)
	}
	if err := client.stopLive(t.Context(), "34020000001320000001", "34020000001320000002"); err != nil {
		t.Fatalf("stopLive() error = %v", err)
	}
	if starts != 1 || stops != 1 {
		t.Fatalf("calls start=%d stop=%d", starts, stops)
	}
}

func TestControlClientReusesConnectionAfterStopResponse(t *testing.T) {
	var connections atomic.Int32
	server := httptest.NewUnstartedServer(http.HandlerFunc(func(writer http.ResponseWriter, _ *http.Request) {
		_, _ = writer.Write([]byte(strings.Repeat("x", 4096)))
	}))
	server.Config.ConnState = func(_ net.Conn, state http.ConnState) {
		if state == http.StateNew {
			connections.Add(1)
		}
	}
	server.Start()
	defer server.Close()

	client := newControlClient(server.URL)
	for range 2 {
		if err := client.stopLive(t.Context(), "34020000001320000001", "34020000001320000002"); err != nil {
			t.Fatalf("stopLive() error = %v", err)
		}
	}
	if connections.Load() != 1 {
		t.Fatalf("HTTP connections = %d, want 1", connections.Load())
	}
}
