package main

import (
	"bytes"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"
)

func TestPublishClaimHTTP(t *testing.T) {
	for _, protocol := range []string{"rtmp", "whip"} {
		t.Run(protocol, func(t *testing.T) {
			registry := newMediaServerRegistry()
			registration := testMediaServerRegistration("media-1", "instance-a", "127.0.0.1")
			if err := registry.register(registration, time.Now()); err != nil {
				t.Fatalf("register() error = %v", err)
			}
			server := newTestInfrastructureServer(t, testConfig(), registry, slog.New(slog.NewTextHandler(io.Discard, nil)))
			streamID := server.allocations.create(streamOperationPublish, protocol, "live/camera", mediaServerInstance{
				serverID: "media-1", instanceID: "instance-a",
			}, time.Now())
			httpServer := httptest.NewServer(server.handler())
			defer httpServer.Close()
			command := map[string]string{
				"stream_id": streamID, "server_id": "media-1", "instance_id": "instance-a",
				"protocol": protocol, "stream_name": "live/camera",
			}

			response := postJSON(t, httpServer.Client(), httpServer.URL+"/internal/publish/claim", command)
			if response.StatusCode != http.StatusNoContent || response.ContentLength != 0 {
				t.Fatalf("status = %d body = %s", response.StatusCode, readBody(t, response))
			}
			response.Body.Close()
			response = postJSON(t, httpServer.Client(), httpServer.URL+"/internal/publish/claim", command)
			assertHTTPError(t, response, http.StatusNotFound, "allocation_not_found")
		})
	}
}

func TestPublishClaimHTTPFencesOfflineAllocatedInstance(t *testing.T) {
	registry := newMediaServerRegistry()
	now := time.Now()
	old := testMediaServerRegistration("media-1", "instance-a", "127.0.0.1")
	if err := registry.register(old, now); err != nil {
		t.Fatalf("register old instance error = %v", err)
	}
	server := newTestInfrastructureServer(t, testConfig(), registry, slog.New(slog.NewTextHandler(io.Discard, nil)))
	streamID := server.allocations.create(streamOperationPublish, "rtmp", "live/camera", mediaServerInstance{
		serverID: old.ServerID, instanceID: old.InstanceID,
	}, now)
	if offline := registry.expire(now.Add(16*time.Second), 15*time.Second); len(offline) != 1 {
		t.Fatalf("offline instances = %+v", offline)
	}
	replacement := testMediaServerRegistration("media-1", "instance-b", "127.0.0.1")
	if err := registry.register(replacement, now.Add(16*time.Second)); err != nil {
		t.Fatalf("register replacement error = %v", err)
	}
	httpServer := httptest.NewServer(server.handler())
	defer httpServer.Close()

	response := postJSON(t, httpServer.Client(), httpServer.URL+"/internal/publish/claim", map[string]string{
		"stream_id": streamID, "server_id": old.ServerID, "instance_id": old.InstanceID,
		"protocol": "rtmp", "stream_name": "live/camera",
	})
	assertHTTPError(t, response, http.StatusGone, "stale_instance")
	stored, ok := storedStreamAllocation(server.allocations, streamID)
	if !ok {
		t.Fatalf("allocation after stale claim = %+v, exists = %v", stored, ok)
	}

	response = postJSON(t, httpServer.Client(), httpServer.URL+"/internal/publish/claim", map[string]string{
		"stream_id": streamID, "server_id": replacement.ServerID, "instance_id": replacement.InstanceID,
		"protocol": "rtmp", "stream_name": "live/camera",
	})
	assertHTTPError(t, response, http.StatusConflict, "allocation_conflict")
	stored, ok = storedStreamAllocation(server.allocations, streamID)
	if !ok {
		t.Fatalf("allocation after replacement claim = %+v, exists = %v", stored, ok)
	}
}

func TestPublishClaimHTTPErrorContract(t *testing.T) {
	registry := newMediaServerRegistry()
	if err := registry.register(testMediaServerRegistration("media-1", "instance-a", "127.0.0.1"), time.Now()); err != nil {
		t.Fatalf("register() error = %v", err)
	}
	server := newTestInfrastructureServer(t, testConfig(), registry, slog.New(slog.NewTextHandler(io.Discard, nil)))
	httpServer := httptest.NewServer(server.handler())
	defer httpServer.Close()
	valid := `{"stream_id":"00000000-0000-4000-8000-000000000001","server_id":"media-1","instance_id":"instance-a","protocol":"rtmp","stream_name":"live/camera"}`

	for name, body := range map[string]string{
		"invalid JSON":     `{`,
		"unknown field":    valid[:len(valid)-1] + `,"extra":true}`,
		"missing ID":       `{"server_id":"media-1","instance_id":"instance-a","protocol":"rtmp","stream_name":"live/camera"}`,
		"invalid ID":       `{"stream_id":"not-a-uuid","server_id":"media-1","instance_id":"instance-a","protocol":"rtmp","stream_name":"live/camera"}`,
		"uppercase ID":     `{"stream_id":"00000000-0000-4000-8000-00000000000A","server_id":"media-1","instance_id":"instance-a","protocol":"rtmp","stream_name":"live/camera"}`,
		"wrong version":    `{"stream_id":"00000000-0000-1000-8000-000000000001","server_id":"media-1","instance_id":"instance-a","protocol":"rtmp","stream_name":"live/camera"}`,
		"wrong variant":    `{"stream_id":"00000000-0000-4000-7000-000000000001","server_id":"media-1","instance_id":"instance-a","protocol":"rtmp","stream_name":"live/camera"}`,
		"legacy direction": `{"stream_id":"00000000-0000-4000-8000-000000000001","server_id":"media-1","instance_id":"instance-a","direction":"input","protocol":"rtmp","stream_name":"live/camera"}`,
		"bad protocol":     `{"stream_id":"00000000-0000-4000-8000-000000000001","server_id":"media-1","instance_id":"instance-a","protocol":"hls","stream_name":"live/camera"}`,
		"empty stream":     `{"stream_id":"00000000-0000-4000-8000-000000000001","server_id":"media-1","instance_id":"instance-a","protocol":"rtmp","stream_name":""}`,
		"extra JSON":       valid + `{}`,
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
		"protocol": "rtmp", "stream_name": "live/camera",
	})
	assertHTTPError(t, response, http.StatusNotFound, "allocation_not_found")
}

func TestPublishClaimHTTPReportsExpiredAllocation(t *testing.T) {
	registry := newMediaServerRegistry()
	if err := registry.register(testMediaServerRegistration("media-1", "instance-a", "127.0.0.1"), time.Now()); err != nil {
		t.Fatalf("register() error = %v", err)
	}
	server := newTestInfrastructureServer(t, testConfig(), registry, slog.New(slog.NewTextHandler(io.Discard, nil)))
	streamID := server.allocations.create(streamOperationPublish, "rtmp", "live/camera", mediaServerInstance{
		serverID: "media-1", instanceID: "instance-a",
	}, time.Now().Add(-streamAllocationTTL))
	httpServer := httptest.NewServer(server.handler())
	defer httpServer.Close()

	response := postJSON(t, httpServer.Client(), httpServer.URL+"/internal/publish/claim", map[string]string{
		"stream_id": streamID, "server_id": "media-1", "instance_id": "instance-a",
		"protocol": "rtmp", "stream_name": "live/camera",
	})
	assertHTTPError(t, response, http.StatusNotFound, "allocation_not_found")
}
