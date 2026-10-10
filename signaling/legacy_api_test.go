package main

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

func TestRetiredGBControlRoutesAreAbsent(t *testing.T) {
	s := testInfrastructure(t)
	handler := s.handler()
	for _, path := range []string{
		"/internal/live/start", "/internal/live/stop",
		"/api/devices/device/channels/channel/start", "/api/devices/device/channels/channel/stop",
	} {
		apiRequest(t, handler, "POST", path, `{}`, http.StatusNotFound)
	}
	apiRequest(t, handler, "POST", "/api/preview/start",
		`{"device_id":"34020000001320000001","channel_id":"34020000001320000002"}`, http.StatusBadRequest)
	apiRequest(t, handler, "POST", "/api/preview/start",
		`{"source_id":"11111111-1111-4111-8111-111111111111"}`, http.StatusNotFound)
}

func TestRTSPSourcePreviewRemainsAvailable(t *testing.T) {
	s := testInfrastructure(t)
	media := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
		if request.URL.Path == "/rtsp/pull/create" {
			writer.WriteHeader(http.StatusCreated)
		} else if request.URL.Path == "/receivers/delete" {
			writer.WriteHeader(http.StatusNoContent)
		} else {
			writer.WriteHeader(http.StatusNotFound)
		}
	}))
	t.Cleanup(media.Close)
	s.media.server.controlURL = media.URL
	handler := s.handler()
	response := apiRequest(t, handler, "POST", "/api/sources",
		`{"stream_name":"rtsp/preview","url":"rtsp://127.0.0.1/camera"}`, http.StatusCreated)
	var source sourceResponse
	if err := json.Unmarshal(response.Body.Bytes(), &source); err != nil {
		t.Fatal(err)
	}
	body := `{"source_id":"` + source.SourceID + `"}`
	apiRequest(t, handler, "POST", "/api/preview/start", body, http.StatusConflict)
	start := apiRequest(t, handler, "POST", "/api/sources/"+source.SourceID+"/start", "", http.StatusCreated)
	var session struct {
		StreamID string `json:"stream_id"`
	}
	if err := json.Unmarshal(start.Body.Bytes(), &session); err != nil || !validUUIDv4(session.StreamID) {
		t.Fatalf("invalid start response: %s", start.Body.String())
	}
	response = apiRequest(t, handler, "POST", "/api/preview/start", body, http.StatusCreated)
	if !strings.Contains(response.Body.String(), "http://127.0.0.1:8080/play/whep/"+session.StreamID) {
		t.Fatal(response.Body.String())
	}
	apiRequest(t, handler, "POST", "/api/sources/"+source.SourceID+"/stop", "", http.StatusNoContent)
	apiRequest(t, handler, "POST", "/api/preview/start", body, http.StatusConflict)
}
