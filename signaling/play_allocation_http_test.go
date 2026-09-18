package main

import (
	"bytes"
	"encoding/json"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"net/url"
	"testing"
	"time"

	"github.com/google/uuid"
)

func TestPlayAllocationHTTP(t *testing.T) {
	for _, test := range []struct {
		name        string
		protocol    string
		expectedURL string
	}{
		{name: "RTMP", protocol: "rtmp", expectedURL: "rtmp://192.0.2.10:1935/live/camera"},
		{name: "RTSP", protocol: "rtsp", expectedURL: "rtsp://192.0.2.10:8554/live/camera"},
	} {
		t.Run(test.name, func(t *testing.T) {
			registry := newMediaServerRegistry()
			if err := registry.register(testMediaServerRegistration("media-1", "instance-a", "192.0.2.10"), time.Now()); err != nil {
				t.Fatalf("register() error = %v", err)
			}
			server := newTestInfrastructureServer(t, testConfig(), registry, slog.New(slog.NewTextHandler(io.Discard, nil)))
			httpServer := httptest.NewServer(server.handler())
			defer httpServer.Close()

			allocate := func() map[string]string {
				response := postJSON(t, httpServer.Client(), httpServer.URL+"/api/play/allocations", map[string]string{
					"protocol": test.protocol, "stream_name": "live/camera",
				})
				defer response.Body.Close()
				if response.StatusCode != http.StatusCreated {
					t.Fatalf("status = %d body = %s", response.StatusCode, readBody(t, response))
				}
				var result map[string]string
				if err := json.NewDecoder(response.Body).Decode(&result); err != nil {
					t.Fatalf("Decode() error = %v", err)
				}
				if len(result) != 2 || result["stream_id"] == "" || result["play_url"] == "" {
					t.Fatalf("response = %#v", result)
				}
				parsedID, err := uuid.Parse(result["stream_id"])
				if err != nil || parsedID.Version() != 4 || parsedID.String() != result["stream_id"] {
					t.Fatalf("stream ID = %q, parse error = %v", result["stream_id"], err)
				}
				playURL, err := url.Parse(result["play_url"])
				if err != nil {
					t.Fatalf("url.Parse() error = %v", err)
				}
				if got := playURL.Scheme + "://" + playURL.Host + playURL.Path; got != test.expectedURL {
					t.Fatalf("play URL base = %q", got)
				}
				if playURL.Query().Get("stream_id") != result["stream_id"] || len(playURL.Query()) != 1 {
					t.Fatalf("play URL query = %q", playURL.RawQuery)
				}
				return result
			}

			first := allocate()
			second := allocate()
			if first["stream_id"] == second["stream_id"] {
				t.Fatalf("duplicate stream ID %q", first["stream_id"])
			}
			allocation, ok := storedStreamAllocation(server.allocations, first["stream_id"])
			if !ok || allocation.operation != streamOperationPlay || allocation.protocol != test.protocol ||
				allocation.streamName != "live/camera" || allocation.serverID != "media-1" || allocation.instanceID != "instance-a" {
				t.Fatalf("stored allocation = %+v, ok = %v", allocation, ok)
			}
		})
	}
}

func TestPlayAllocationHTTPRejectsInvalidRequests(t *testing.T) {
	registry := newMediaServerRegistry()
	if err := registry.register(testMediaServerRegistration("media-1", "instance-a", "127.0.0.1"), time.Now()); err != nil {
		t.Fatalf("register() error = %v", err)
	}
	server := newTestInfrastructureServer(t, testConfig(), registry, slog.New(slog.NewTextHandler(io.Discard, nil)))
	httpServer := httptest.NewServer(server.handler())
	defer httpServer.Close()

	for name, body := range map[string]string{
		"invalid JSON": `{`,
		"bad protocol": `{"protocol":"hls","stream_name":"live/camera"}`,
		"empty stream": `{"protocol":"rtsp","stream_name":""}`,
	} {
		t.Run(name, func(t *testing.T) {
			request, err := http.NewRequest(http.MethodPost, httpServer.URL+"/api/play/allocations", bytes.NewBufferString(body))
			if err != nil {
				t.Fatalf("NewRequest() error = %v", err)
			}
			request.Header.Set("Content-Type", "application/json")
			response, err := httpServer.Client().Do(request)
			if err != nil {
				t.Fatalf("Do() error = %v", err)
			}
			assertHTTPError(t, response, http.StatusBadRequest, "invalid_request")
		})
	}
}

func TestPlayAllocationHTTPRequiresOnlineMediaServer(t *testing.T) {
	server := newTestInfrastructureServer(t, testConfig(), newMediaServerRegistry(), slog.New(slog.NewTextHandler(io.Discard, nil)))
	httpServer := httptest.NewServer(server.handler())
	defer httpServer.Close()

	response := postJSON(t, httpServer.Client(), httpServer.URL+"/api/play/allocations", map[string]string{
		"protocol": "rtsp", "stream_name": "live/camera",
	})
	assertHTTPError(t, response, http.StatusServiceUnavailable, "no_media_server")
}
