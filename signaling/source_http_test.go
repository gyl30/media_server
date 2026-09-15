package main

import (
	"encoding/json"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

func TestSourceHTTPCreateListAndCredentialUpdates(t *testing.T) {
	server := newTestInfrastructureServer(
		t, testConfig(), newMediaServerRegistry(), slog.New(slog.NewTextHandler(io.Discard, nil)))
	handler := server.handler()
	secret := "not-returned-secret"
	response := sourceRequest(t, handler, http.MethodPost, "/api/sources",
		`{"stream_name":"live/camera","url":"rtsp://camera.example/live","username":"admin","password":"`+secret+`"}`,
		"application/json")
	if response.Code != http.StatusCreated {
		t.Fatalf("create status = %d body = %s", response.Code, response.Body.String())
	}
	if strings.Contains(response.Body.String(), "password") || strings.Contains(response.Body.String(), secret) {
		t.Fatalf("create response leaks password: %s", response.Body.String())
	}
	var created sourceResponse
	if err := json.Unmarshal(response.Body.Bytes(), &created); err != nil {
		t.Fatalf("decode create response: %v", err)
	}
	if !validUUIDv4(created.SourceID) || created.StreamName != "live/camera" || created.URL != "rtsp://camera.example/live" ||
		created.Username != "admin" || created.DesiredState != sourceDesiredStopped {
		t.Fatalf("create response = %+v", created)
	}

	response = sourceRequest(t, handler, http.MethodGet, "/api/sources", "", "")
	if response.Code != http.StatusOK || strings.Contains(response.Body.String(), "password") || strings.Contains(response.Body.String(), secret) {
		t.Fatalf("list status/body = %d %s", response.Code, response.Body.String())
	}
	var listed struct {
		Sources []sourceResponse `json:"sources"`
	}
	if err := json.Unmarshal(response.Body.Bytes(), &listed); err != nil || len(listed.Sources) != 1 || listed.Sources[0] != created {
		t.Fatalf("list response = %+v, %v", listed, err)
	}

	response = sourceRequest(t, handler, http.MethodPatch, "/api/sources/"+created.SourceID,
		`{"url":"rtsp://camera.example/updated"}`, "application/json")
	if response.Code != http.StatusOK || strings.Contains(response.Body.String(), secret) {
		t.Fatalf("patch URL status/body = %d %s", response.Code, response.Body.String())
	}
	stored, err := server.sources.get(t.Context(), created.SourceID)
	if err != nil || stored.password != secret || stored.url != "rtsp://camera.example/updated" {
		t.Fatalf("stored after URL patch = %+v, %v", stored, err)
	}

	response = sourceRequest(t, handler, http.MethodPatch, "/api/sources/"+created.SourceID,
		`{"username":"","password":""}`, "application/json")
	if response.Code != http.StatusOK {
		t.Fatalf("clear credentials status/body = %d %s", response.Code, response.Body.String())
	}
	stored, err = server.sources.get(t.Context(), created.SourceID)
	if err != nil || stored.username != "" || stored.password != "" {
		t.Fatalf("stored after credential clear = %+v, %v", stored, err)
	}

	response = sourceRequest(t, handler, http.MethodPatch, "/api/sources/"+created.SourceID,
		`{"username":"operator","password":"replacement"}`, "application/json")
	if response.Code != http.StatusOK || strings.Contains(response.Body.String(), "replacement") {
		t.Fatalf("replace credentials status/body = %d %s", response.Code, response.Body.String())
	}
	stored, err = server.sources.get(t.Context(), created.SourceID)
	if err != nil || stored.username != "operator" || stored.password != "replacement" {
		t.Fatalf("stored after credential replace = %+v, %v", stored, err)
	}

	response = sourceRequest(t, handler, http.MethodDelete, "/api/sources/"+created.SourceID, "", "")
	if response.Code != http.StatusNoContent || response.Body.Len() != 0 {
		t.Fatalf("delete status/body = %d %s", response.Code, response.Body.String())
	}
	response = sourceRequest(t, handler, http.MethodGet, "/api/sources", "", "")
	if response.Code != http.StatusOK || response.Body.String() != "{\"sources\":[]}\n" {
		t.Fatalf("empty list status/body = %d %s", response.Code, response.Body.String())
	}
}

func TestSourceHTTPAcceptsUnauthenticatedSource(t *testing.T) {
	server := newTestInfrastructureServer(
		t, testConfig(), newMediaServerRegistry(), slog.New(slog.NewTextHandler(io.Discard, nil)))
	response := sourceRequest(t, server.handler(), http.MethodPost, "/api/sources",
		`{"stream_name":"live/public","url":"rtsp://camera.example/live"}`, "application/json")
	if response.Code != http.StatusCreated {
		t.Fatalf("create status/body = %d %s", response.Code, response.Body.String())
	}
}

func TestSourceHTTPRejectsInvalidCreateRequests(t *testing.T) {
	server := newTestInfrastructureServer(
		t, testConfig(), newMediaServerRegistry(), slog.New(slog.NewTextHandler(io.Discard, nil)))
	tests := []struct {
		name        string
		body        string
		contentType string
	}{
		{name: "missing stream name", body: `{"url":"rtsp://camera.example/live"}`, contentType: "application/json"},
		{name: "missing URL", body: `{"stream_name":"live/camera"}`, contentType: "application/json"},
		{name: "password without username", body: `{"stream_name":"live/camera","url":"rtsp://camera.example/live","password":"secret"}`, contentType: "application/json"},
		{name: "empty password without username", body: `{"stream_name":"live/camera","url":"rtsp://camera.example/live","password":""}`, contentType: "application/json"},
		{name: "userinfo", body: `{"stream_name":"live/camera","url":"rtsp://admin:secret@camera.example/live"}`, contentType: "application/json"},
		{name: "wrong scheme", body: `{"stream_name":"live/camera","url":"http://camera.example/live"}`, contentType: "application/json"},
		{name: "missing host", body: `{"stream_name":"live/camera","url":"rtsp:///live"}`, contentType: "application/json"},
		{name: "zero port", body: `{"stream_name":"live/camera","url":"rtsp://camera.example:0/live"}`, contentType: "application/json"},
		{name: "empty port", body: `{"stream_name":"live/camera","url":"rtsp://camera.example:/live"}`, contentType: "application/json"},
		{name: "empty IPv6 port", body: `{"stream_name":"live/camera","url":"rtsp://[2001:db8::1]:/live"}`, contentType: "application/json"},
		{name: "overflow port", body: `{"stream_name":"live/camera","url":"rtsp://camera.example:65536/live"}`, contentType: "application/json"},
		{name: "unknown field", body: `{"stream_name":"live/camera","url":"rtsp://camera.example/live","extra":true}`, contentType: "application/json"},
		{name: "wrong type", body: `{"stream_name":1,"url":"rtsp://camera.example/live"}`, contentType: "application/json"},
		{name: "null credential", body: `{"stream_name":"live/camera","url":"rtsp://camera.example/live","username":null}`, contentType: "application/json"},
		{name: "trailing JSON", body: `{"stream_name":"live/camera","url":"rtsp://camera.example/live"}{}`, contentType: "application/json"},
		{name: "wrong content type", body: `{"stream_name":"live/camera","url":"rtsp://camera.example/live"}`, contentType: "text/plain"},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			response := sourceRequest(t, server.handler(), http.MethodPost, "/api/sources", test.body, test.contentType)
			if response.Code != http.StatusBadRequest {
				t.Fatalf("status/body = %d %s", response.Code, response.Body.String())
			}
		})
	}
}

func TestSourceHTTPConflictsAndMissingSources(t *testing.T) {
	server := newTestInfrastructureServer(
		t, testConfig(), newMediaServerRegistry(), slog.New(slog.NewTextHandler(io.Discard, nil)))
	handler := server.handler()
	response := sourceRequest(t, handler, http.MethodPost, "/api/sources",
		`{"stream_name":"live/camera","url":"rtsp://camera.example/live"}`, "application/json")
	if response.Code != http.StatusCreated {
		t.Fatalf("initial create status = %d", response.Code)
	}
	var created sourceResponse
	if err := json.Unmarshal(response.Body.Bytes(), &created); err != nil {
		t.Fatalf("decode create response: %v", err)
	}
	response = sourceRequest(t, handler, http.MethodPost, "/api/sources",
		`{"stream_name":"live/camera","url":"rtsp://other.example/live"}`, "application/json")
	if response.Code != http.StatusConflict {
		t.Fatalf("duplicate status/body = %d %s", response.Code, response.Body.String())
	}

	missingID := "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"
	response = sourceRequest(t, handler, http.MethodPatch, "/api/sources/"+missingID,
		`{"url":"rtsp://camera.example/new"}`, "application/json")
	if response.Code != http.StatusNotFound {
		t.Fatalf("missing patch status/body = %d %s", response.Code, response.Body.String())
	}
	response = sourceRequest(t, handler, http.MethodDelete, "/api/sources/"+missingID, "", "")
	if response.Code != http.StatusNotFound {
		t.Fatalf("missing delete status/body = %d %s", response.Code, response.Body.String())
	}
	response = sourceRequest(t, handler, http.MethodPatch, "/api/sources/"+created.SourceID, `{}`, "application/json")
	if response.Code != http.StatusBadRequest {
		t.Fatalf("empty patch status/body = %d %s", response.Code, response.Body.String())
	}
	response = sourceRequest(t, handler, http.MethodPatch, "/api/sources/not-a-uuid",
		`{"url":"rtsp://camera.example/new"}`, "application/json")
	if response.Code != http.StatusBadRequest {
		t.Fatalf("invalid ID status/body = %d %s", response.Code, response.Body.String())
	}
	response = sourceRequest(t, handler, http.MethodPut, "/api/sources", `{}`, "application/json")
	if response.Code != http.StatusMethodNotAllowed {
		t.Fatalf("wrong method status/body = %d %s", response.Code, response.Body.String())
	}
}

func sourceRequest(t *testing.T, handler http.Handler, method, target, body, contentType string) *httptest.ResponseRecorder {
	t.Helper()
	request := httptest.NewRequest(method, target, strings.NewReader(body))
	if contentType != "" {
		request.Header.Set("Content-Type", contentType)
	}
	response := httptest.NewRecorder()
	handler.ServeHTTP(response, request)
	return response
}
