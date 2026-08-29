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

func TestMediaServerInfrastructureHTTP(t *testing.T) {
	registry := newMediaServerRegistry()
	server := newInfrastructureServer(config{mediaServerTimeout: 15 * time.Second}, registry, slog.New(slog.NewTextHandler(io.Discard, nil)))
	httpServer := httptest.NewServer(server.handler())
	defer httpServer.Close()
	client := httpServer.Client()

	registration := testMediaServerRegistration("media-1", "instance-a", "127.0.0.1")
	registration.ControlURL += "/"
	response := postJSON(t, client, httpServer.URL+"/internal/media-servers/register", registration)
	if response.StatusCode != http.StatusOK {
		t.Fatalf("registration status = %d body = %s", response.StatusCode, readBody(t, response))
	}
	response.Body.Close()
	instance, ok := registry.selectOnline()
	if !ok || instance.controlURL != registration.ControlURL[:len(registration.ControlURL)-1] {
		t.Fatalf("registered control URL = %q", instance.controlURL)
	}

	response = postJSON(t, client, httpServer.URL+"/internal/media-servers/register", registration)
	assertHTTPError(t, response, http.StatusConflict, "instance_conflict")

	response = postJSON(t, client, httpServer.URL+"/internal/media-servers/heartbeat", mediaServerHeartbeat{
		ServerID: registration.ServerID, InstanceID: registration.InstanceID,
	})
	if response.StatusCode != http.StatusOK {
		t.Fatalf("heartbeat status = %d body = %s", response.StatusCode, readBody(t, response))
	}
	response.Body.Close()

	response = postJSON(t, client, httpServer.URL+"/internal/media-servers/heartbeat", mediaServerHeartbeat{
		ServerID: "unknown", InstanceID: "unknown",
	})
	assertHTTPError(t, response, http.StatusGone, "stale_instance")
}

func TestMediaServerInfrastructureHTTPRejectsMalformedRequests(t *testing.T) {
	registry := newMediaServerRegistry()
	server := newInfrastructureServer(config{mediaServerTimeout: 15 * time.Second}, registry, slog.New(slog.NewTextHandler(io.Discard, nil)))
	httpServer := httptest.NewServer(server.handler())
	defer httpServer.Close()

	for name, body := range map[string]string{
		"invalid JSON":     `{`,
		"unknown field":    `{"server_id":"media-1","unknown":true}`,
		"invalid URL":      `{"server_id":"media-1","instance_id":"a","control_url":"file:///tmp/a","media_ip":"127.0.0.1"}`,
		"invalid media IP": `{"server_id":"media-1","instance_id":"a","control_url":"http://127.0.0.1:8080","media_ip":"bad"}`,
	} {
		t.Run(name, func(t *testing.T) {
			request, err := http.NewRequest(http.MethodPost, httpServer.URL+"/internal/media-servers/register", bytes.NewBufferString(body))
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

func postJSON(t *testing.T, client *http.Client, url string, value any) *http.Response {
	t.Helper()
	body, err := json.Marshal(value)
	if err != nil {
		t.Fatalf("json.Marshal() error = %v", err)
	}
	request, err := http.NewRequest(http.MethodPost, url, bytes.NewReader(body))
	if err != nil {
		t.Fatalf("NewRequest() error = %v", err)
	}
	request.Header.Set("Content-Type", "application/json")
	response, err := client.Do(request)
	if err != nil {
		t.Fatalf("Do() error = %v", err)
	}
	return response
}

func assertHTTPError(t *testing.T, response *http.Response, status int, code string) {
	t.Helper()
	defer response.Body.Close()
	if response.StatusCode != status {
		t.Fatalf("status = %d body = %s", response.StatusCode, readBody(t, response))
	}
	var result struct {
		Error string `json:"error"`
	}
	if err := json.NewDecoder(response.Body).Decode(&result); err != nil {
		t.Fatalf("Decode() error = %v", err)
	}
	if result.Error != code {
		t.Fatalf("error = %q", result.Error)
	}
}

func readBody(t *testing.T, response *http.Response) string {
	t.Helper()
	body, err := io.ReadAll(response.Body)
	if err != nil {
		t.Fatalf("ReadAll() error = %v", err)
	}
	return string(body)
}
