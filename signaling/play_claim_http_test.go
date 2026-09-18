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

func TestPlayClaimHTTPClaimsOnce(t *testing.T) {
	for _, protocol := range []string{"rtsp", "http-flv"} {
		t.Run(protocol, func(t *testing.T) {
			registry := newMediaServerRegistry()
			serverInstance := testMediaServerRegistration("media-1", "instance-a", "127.0.0.1")
			if err := registry.register(serverInstance, time.Now()); err != nil {
				t.Fatalf("register() error = %v", err)
			}
			server := newTestInfrastructureServer(t, testConfig(), registry, slog.New(slog.NewTextHandler(io.Discard, nil)))
			streamID := server.allocations.create(streamOperationPlay, protocol, "live/camera", mediaServerInstance{
				serverID: serverInstance.ServerID, instanceID: serverInstance.InstanceID,
			}, time.Now())
			httpServer := httptest.NewServer(server.handler())
			defer httpServer.Close()
			command := map[string]string{
				"stream_id": streamID, "server_id": "media-1", "instance_id": "instance-a",
				"protocol": protocol, "stream_name": "live/camera",
			}

			response := postJSON(t, httpServer.Client(), httpServer.URL+"/internal/play/claim", command)
			if response.StatusCode != http.StatusNoContent || response.ContentLength != 0 {
				t.Fatalf("status = %d body = %s", response.StatusCode, readBody(t, response))
			}
			response.Body.Close()
			response = postJSON(t, httpServer.Client(), httpServer.URL+"/internal/play/claim", command)
			assertHTTPError(t, response, http.StatusNotFound, "allocation_not_found")
		})
	}
}

func TestPlayClaimHTTPOperationConflictsDoNotConsumeAllocation(t *testing.T) {
	registry := newMediaServerRegistry()
	registration := testMediaServerRegistration("media-1", "instance-a", "127.0.0.1")
	if err := registry.register(registration, time.Now()); err != nil {
		t.Fatalf("register() error = %v", err)
	}
	server := newTestInfrastructureServer(t, testConfig(), registry, slog.New(slog.NewTextHandler(io.Discard, nil)))
	httpServer := httptest.NewServer(server.handler())
	defer httpServer.Close()
	instance := mediaServerInstance{serverID: registration.ServerID, instanceID: registration.InstanceID}

	for _, test := range []struct {
		name, allocationPath, acceptedPath string
		operation                          streamOperation
	}{
		{name: "publish allocation with play claim", operation: streamOperationPublish, allocationPath: "/internal/play/claim", acceptedPath: "/internal/publish/claim"},
		{name: "play allocation with publish claim", operation: streamOperationPlay, allocationPath: "/internal/publish/claim", acceptedPath: "/internal/play/claim"},
	} {
		t.Run(test.name, func(t *testing.T) {
			streamID := server.allocations.create(test.operation, "rtmp", "live/camera", instance, time.Now())
			command := map[string]string{
				"stream_id": streamID, "server_id": "media-1", "instance_id": "instance-a",
				"protocol": "rtmp", "stream_name": "live/camera",
			}
			response := postJSON(t, httpServer.Client(), httpServer.URL+test.allocationPath, command)
			assertHTTPError(t, response, http.StatusConflict, "allocation_conflict")
			response = postJSON(t, httpServer.Client(), httpServer.URL+test.acceptedPath, command)
			if response.StatusCode != http.StatusNoContent {
				t.Fatalf("status = %d body = %s", response.StatusCode, readBody(t, response))
			}
			response.Body.Close()
		})
	}
}

func TestPlayClaimHTTPProtocolConflictDoesNotConsumeHTTPFLVAllocation(t *testing.T) {
	registry := newMediaServerRegistry()
	registration := testMediaServerRegistration("media-1", "instance-a", "127.0.0.1")
	if err := registry.register(registration, time.Now()); err != nil {
		t.Fatalf("register() error = %v", err)
	}
	server := newTestInfrastructureServer(t, testConfig(), registry, slog.New(slog.NewTextHandler(io.Discard, nil)))
	streamID := server.allocations.create(streamOperationPlay, "http-flv", "live/camera", mediaServerInstance{
		serverID: registration.ServerID, instanceID: registration.InstanceID,
	}, time.Now())
	httpServer := httptest.NewServer(server.handler())
	defer httpServer.Close()
	command := map[string]string{
		"stream_id": streamID, "server_id": "media-1", "instance_id": "instance-a",
		"protocol": "rtsp", "stream_name": "live/camera",
	}
	response := postJSON(t, httpServer.Client(), httpServer.URL+"/internal/play/claim", command)
	assertHTTPError(t, response, http.StatusConflict, "allocation_conflict")
	command["protocol"] = "http-flv"
	response = postJSON(t, httpServer.Client(), httpServer.URL+"/internal/play/claim", command)
	if response.StatusCode != http.StatusNoContent {
		t.Fatalf("status = %d body = %s", response.StatusCode, readBody(t, response))
	}
	response.Body.Close()
}

func TestPlayClaimHTTPFencesStaleInstance(t *testing.T) {
	registry := newMediaServerRegistry()
	now := time.Now()
	registration := testMediaServerRegistration("media-1", "instance-a", "127.0.0.1")
	if err := registry.register(registration, now); err != nil {
		t.Fatalf("register() error = %v", err)
	}
	server := newTestInfrastructureServer(t, testConfig(), registry, slog.New(slog.NewTextHandler(io.Discard, nil)))
	streamID := server.allocations.create(streamOperationPlay, "rtmp", "live/camera", mediaServerInstance{
		serverID: registration.ServerID, instanceID: registration.InstanceID,
	}, now)
	if offline := registry.expire(now.Add(16*time.Second), 15*time.Second); len(offline) != 1 {
		t.Fatalf("offline instances = %+v", offline)
	}
	httpServer := httptest.NewServer(server.handler())
	defer httpServer.Close()

	response := postJSON(t, httpServer.Client(), httpServer.URL+"/internal/play/claim", map[string]string{
		"stream_id": streamID, "server_id": "media-1", "instance_id": "instance-a",
		"protocol": "rtmp", "stream_name": "live/camera",
	})
	assertHTTPError(t, response, http.StatusGone, "stale_instance")
	if _, ok := storedStreamAllocation(server.allocations, streamID); !ok {
		t.Fatal("stale claim consumed allocation")
	}
}

func TestPlayClaimHTTPRejectsInvalidRequests(t *testing.T) {
	registry := newMediaServerRegistry()
	if err := registry.register(testMediaServerRegistration("media-1", "instance-a", "127.0.0.1"), time.Now()); err != nil {
		t.Fatalf("register() error = %v", err)
	}
	server := newTestInfrastructureServer(t, testConfig(), registry, slog.New(slog.NewTextHandler(io.Discard, nil)))
	httpServer := httptest.NewServer(server.handler())
	defer httpServer.Close()

	for name, body := range map[string]string{
		"invalid UUID": `{"stream_id":"bad","server_id":"media-1","instance_id":"instance-a","protocol":"rtmp","stream_name":"live/camera"}`,
		"bad protocol": `{"stream_id":"00000000-0000-4000-8000-000000000001","server_id":"media-1","instance_id":"instance-a","protocol":"hls","stream_name":"live/camera"}`,
		"empty stream": `{"stream_id":"00000000-0000-4000-8000-000000000001","server_id":"media-1","instance_id":"instance-a","protocol":"rtmp","stream_name":""}`,
	} {
		t.Run(name, func(t *testing.T) {
			request, err := http.NewRequest(http.MethodPost, httpServer.URL+"/internal/play/claim", bytes.NewBufferString(body))
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

func TestPlayClaimHTTPReportsExpiredAllocation(t *testing.T) {
	registry := newMediaServerRegistry()
	if err := registry.register(testMediaServerRegistration("media-1", "instance-a", "127.0.0.1"), time.Now()); err != nil {
		t.Fatalf("register() error = %v", err)
	}
	server := newTestInfrastructureServer(t, testConfig(), registry, slog.New(slog.NewTextHandler(io.Discard, nil)))
	streamID := server.allocations.create(streamOperationPlay, "rtmp", "live/camera", mediaServerInstance{
		serverID: "media-1", instanceID: "instance-a",
	}, time.Now().Add(-streamAllocationTTL))
	httpServer := httptest.NewServer(server.handler())
	defer httpServer.Close()

	response := postJSON(t, httpServer.Client(), httpServer.URL+"/internal/play/claim", map[string]string{
		"stream_id": streamID, "server_id": "media-1", "instance_id": "instance-a",
		"protocol": "rtmp", "stream_name": "live/camera",
	})
	assertHTTPError(t, response, http.StatusNotFound, "allocation_not_found")
}
