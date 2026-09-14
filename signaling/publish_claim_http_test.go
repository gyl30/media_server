package main

import (
	"bytes"
	"encoding/json"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"
)

func TestPublishClaimHTTP(t *testing.T) {
	registry := newMediaServerRegistry()
	registration := testMediaServerRegistration("media-1", "instance-a", "127.0.0.1")
	if err := registry.register(registration, time.Now()); err != nil {
		t.Fatalf("register() error = %v", err)
	}
	server := newTestInfrastructureServer(t, testConfig(), registry, slog.New(slog.NewTextHandler(io.Discard, nil)))
	allocation := server.allocations.create("rtmp", "live/camera", mediaServerInstance{
		serverID: "media-1", instanceID: "instance-a",
	}, time.Now())
	httpServer := httptest.NewServer(server.handler())
	defer httpServer.Close()
	command := map[string]string{
		"stream_id": allocation.streamID, "server_id": "media-1", "instance_id": "instance-a",
		"direction": "input", "protocol": "rtmp", "stream_name": "live/camera",
	}

	response := postJSON(t, httpServer.Client(), httpServer.URL+"/internal/publish/claim", command)
	defer response.Body.Close()
	if response.StatusCode != http.StatusOK {
		t.Fatalf("status = %d body = %s", response.StatusCode, readBody(t, response))
	}
	var result map[string]string
	if err := json.NewDecoder(response.Body).Decode(&result); err != nil {
		t.Fatalf("Decode() error = %v", err)
	}
	if len(result) != 1 || result["result"] != "ok" {
		t.Fatalf("response = %#v", result)
	}
	response = postJSON(t, httpServer.Client(), httpServer.URL+"/internal/publish/claim", command)
	assertHTTPError(t, response, http.StatusConflict, "allocation_conflict")
}

func TestPublishClaimHTTPErrorContract(t *testing.T) {
	server := newTestInfrastructureServer(t, testConfig(), newMediaServerRegistry(), slog.New(slog.NewTextHandler(io.Discard, nil)))
	httpServer := httptest.NewServer(server.handler())
	defer httpServer.Close()
	valid := `{"stream_id":"00000000-0000-4000-8000-000000000001","server_id":"media-1","instance_id":"instance-a","direction":"input","protocol":"rtmp","stream_name":"live/camera"}`

	for name, body := range map[string]string{
		"invalid JSON":  `{`,
		"unknown field": valid[:len(valid)-1] + `,"extra":true}`,
		"missing ID":    `{"server_id":"media-1","instance_id":"instance-a","direction":"input","protocol":"rtmp","stream_name":"live/camera"}`,
		"invalid ID":    `{"stream_id":"not-a-uuid","server_id":"media-1","instance_id":"instance-a","direction":"input","protocol":"rtmp","stream_name":"live/camera"}`,
		"uppercase ID":  `{"stream_id":"00000000-0000-4000-8000-00000000000A","server_id":"media-1","instance_id":"instance-a","direction":"input","protocol":"rtmp","stream_name":"live/camera"}`,
		"wrong version": `{"stream_id":"00000000-0000-1000-8000-000000000001","server_id":"media-1","instance_id":"instance-a","direction":"input","protocol":"rtmp","stream_name":"live/camera"}`,
		"wrong variant": `{"stream_id":"00000000-0000-4000-7000-000000000001","server_id":"media-1","instance_id":"instance-a","direction":"input","protocol":"rtmp","stream_name":"live/camera"}`,
		"bad direction": `{"stream_id":"00000000-0000-4000-8000-000000000001","server_id":"media-1","instance_id":"instance-a","direction":"output","protocol":"rtmp","stream_name":"live/camera"}`,
		"bad protocol":  `{"stream_id":"00000000-0000-4000-8000-000000000001","server_id":"media-1","instance_id":"instance-a","direction":"input","protocol":"hls","stream_name":"live/camera"}`,
		"empty stream":  `{"stream_id":"00000000-0000-4000-8000-000000000001","server_id":"media-1","instance_id":"instance-a","direction":"input","protocol":"rtmp","stream_name":""}`,
		"extra JSON":    valid + `{}`,
	} {
		t.Run(name, func(t *testing.T) {
			request, err := http.NewRequest(http.MethodPost, httpServer.URL+"/internal/publish/claim", bytes.NewBufferString(body))
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

	response := postJSON(t, httpServer.Client(), httpServer.URL+"/internal/publish/claim", map[string]string{
		"stream_id": "00000000-0000-4000-8000-000000000001", "server_id": "media-1", "instance_id": "instance-a",
		"direction": "input", "protocol": "rtmp", "stream_name": "live/camera",
	})
	assertHTTPError(t, response, http.StatusNotFound, "allocation_not_found")
}

func TestPublishClaimHTTPReportsExpiredAllocation(t *testing.T) {
	server := newTestInfrastructureServer(t, testConfig(), newMediaServerRegistry(), slog.New(slog.NewTextHandler(io.Discard, nil)))
	allocation := server.allocations.create("rtmp", "live/camera", mediaServerInstance{
		serverID: "media-1", instanceID: "instance-a",
	}, time.Now().Add(-publishAllocationTTL))
	httpServer := httptest.NewServer(server.handler())
	defer httpServer.Close()

	response := postJSON(t, httpServer.Client(), httpServer.URL+"/internal/publish/claim", map[string]string{
		"stream_id": allocation.streamID, "server_id": "media-1", "instance_id": "instance-a",
		"direction": "input", "protocol": "rtmp", "stream_name": "live/camera",
	})
	assertHTTPError(t, response, http.StatusGone, "allocation_expired")
}
