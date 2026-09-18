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

func TestPublishAllocationHTTP(t *testing.T) {
	for _, test := range []struct {
		name        string
		protocol    string
		mediaIP     string
		expectedURL string
	}{
		{name: "RTMP", protocol: "rtmp", mediaIP: "192.0.2.10", expectedURL: "rtmp://192.0.2.10:1935/live/camera"},
		{name: "RTSP IPv6", protocol: "rtsp", mediaIP: "2001:db8::10", expectedURL: "rtsp://[2001:db8::10]:8554/live/camera"},
	} {
		t.Run(test.name, func(t *testing.T) {
			registry := newMediaServerRegistry()
			registration := testMediaServerRegistration("media-1", "instance-a", test.mediaIP)
			if err := registry.register(registration, time.Now()); err != nil {
				t.Fatalf("register() error = %v", err)
			}
			server := newTestInfrastructureServer(t, testConfig(), registry, slog.New(slog.NewTextHandler(io.Discard, nil)))
			httpServer := httptest.NewServer(server.handler())
			defer httpServer.Close()

			response := postJSON(t, httpServer.Client(), httpServer.URL+"/api/publish/allocations", map[string]string{
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
			if len(result) != 2 || result["stream_id"] == "" || result["publish_url"] == "" {
				t.Fatalf("response = %#v", result)
			}
			parsedID, err := uuid.Parse(result["stream_id"])
			if err != nil || parsedID.Version() != 4 || parsedID.String() != result["stream_id"] {
				t.Fatalf("stream ID = %q, parse error = %v", result["stream_id"], err)
			}
			publishURL, err := url.Parse(result["publish_url"])
			if err != nil {
				t.Fatalf("url.Parse() error = %v", err)
			}
			if got := publishURL.Scheme + "://" + publishURL.Host + publishURL.Path; got != test.expectedURL {
				t.Fatalf("publish URL base = %q", got)
			}
			if publishURL.Query().Get("stream_id") != result["stream_id"] || len(publishURL.Query()) != 1 {
				t.Fatalf("publish URL query = %q", publishURL.RawQuery)
			}

			allocation, ok := storedStreamAllocation(server.allocations, result["stream_id"])
			if !ok || allocation.protocol != test.protocol ||
				allocation.streamName != "live/camera" || allocation.serverID != "media-1" || allocation.instanceID != "instance-a" {
				t.Fatalf("stored allocation = %+v, ok = %v", allocation, ok)
			}
		})
	}
}

func TestPublishAllocationHTTPRejectsInvalidRequests(t *testing.T) {
	registry := newMediaServerRegistry()
	registration := testMediaServerRegistration("media-1", "instance-a", "127.0.0.1")
	if err := registry.register(registration, time.Now()); err != nil {
		t.Fatalf("register() error = %v", err)
	}
	server := newTestInfrastructureServer(t, testConfig(), registry, slog.New(slog.NewTextHandler(io.Discard, nil)))
	httpServer := httptest.NewServer(server.handler())
	defer httpServer.Close()

	for name, body := range map[string]string{
		"invalid JSON":   `{`,
		"unknown field":  `{"protocol":"rtmp","stream_name":"live/camera","extra":true}`,
		"empty protocol": `{"protocol":"","stream_name":"live/camera"}`,
		"empty name":     `{"protocol":"rtmp","stream_name":""}`,
		"unsupported":    `{"protocol":"hls","stream_name":"live/camera"}`,
		"extra JSON":     `{"protocol":"rtmp","stream_name":"live/camera"}{}`,
	} {
		t.Run(name, func(t *testing.T) {
			request, err := http.NewRequest(http.MethodPost, httpServer.URL+"/api/publish/allocations", bytes.NewBufferString(body))
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

	request, err := http.NewRequest(http.MethodPost, httpServer.URL+"/api/publish/allocations", bytes.NewBufferString(`{"protocol":"rtmp","stream_name":"live/camera"}`))
	if err != nil {
		t.Fatalf("NewRequest() error = %v", err)
	}
	request.Header.Set("Content-Type", "text/plain")
	response, err := httpServer.Client().Do(request)
	if err != nil {
		t.Fatalf("Do() error = %v", err)
	}
	assertHTTPError(t, response, http.StatusBadRequest, "invalid_request")
}

func TestPublishAllocationHTTPRequiresOnlineMediaServer(t *testing.T) {
	server := newTestInfrastructureServer(t, testConfig(), newMediaServerRegistry(), slog.New(slog.NewTextHandler(io.Discard, nil)))
	httpServer := httptest.NewServer(server.handler())
	defer httpServer.Close()

	response := postJSON(t, httpServer.Client(), httpServer.URL+"/api/publish/allocations", map[string]string{
		"protocol": "rtmp", "stream_name": "live/camera",
	})
	assertHTTPError(t, response, http.StatusServiceUnavailable, "no_media_server")
}
