package main

import (
	"encoding/json"
	"net/http"
	"strings"
	"testing"
)

func TestRetiredControlRoutesAreAbsent(t *testing.T) {
	s := testInfrastructure(t)
	handler := s.handler()
	for _, path := range []string{
		"/internal/live/start", "/internal/live/stop",
		"/api/devices/device/channels/channel/start", "/api/devices/device/channels/channel/stop",
		"/api/preview/start",
	} {
		apiRequest(t, handler, "POST", path, `{}`, http.StatusNotFound)
	}
}

func TestRTSPSourcePlayAvailable(t *testing.T) {
	s := testInfrastructure(t)
	media := &fakePullMedia{}
	media.serve(t, s)
	handler := s.handler()
	response := apiRequest(t, handler, "POST", "/api/sources",
		`{"stream_name":"rtsp/preview","url":"rtsp://127.0.0.1/camera"}`, http.StatusCreated)
	var source sourceResponse
	if err := json.Unmarshal(response.Body.Bytes(), &source); err != nil {
		t.Fatal(err)
	}
	if source.DesiredState != sourceDesiredRunning || source.Session == nil || source.Session.State != "created" {
		t.Fatalf("create did not start pulling: %s", response.Body.String())
	}
	body := `{"source":{"source_id":"` + source.SourceID + `"}}`
	apiRequest(t, handler, "POST", "/api/play", body, http.StatusConflict)
	media.ready = true
	if err := s.reconcileMedia(t.Context()); err != nil {
		t.Fatal(err)
	}
	apiRequest(t, handler, "POST", "/api/play", body, http.StatusCreated)
	apiRequest(t, handler, "POST", "/api/sources/"+source.SourceID+"/stop", "", http.StatusNoContent)
	apiRequest(t, handler, "POST", "/api/play", body, http.StatusConflict)
	start := apiRequest(t, handler, "POST", "/api/sources/"+source.SourceID+"/start", "", http.StatusCreated)
	var session struct {
		StreamID string `json:"stream_id"`
	}
	if err := json.Unmarshal(start.Body.Bytes(), &session); err != nil || !validUUIDv4(session.StreamID) {
		t.Fatalf("invalid start response: %s", start.Body.String())
	}
	apiRequest(t, handler, "POST", "/api/play", body, http.StatusConflict)
	if err := s.reconcileMedia(t.Context()); err != nil {
		t.Fatal(err)
	}
	response = apiRequest(t, handler, "POST", "/api/play", body, http.StatusCreated)
	if !strings.Contains(response.Body.String(), "http://127.0.0.1:8080/play/whep/"+session.StreamID) {
		t.Fatal(response.Body.String())
	}
	apiRequest(t, handler, "POST", "/api/sources/"+source.SourceID+"/stop", "", http.StatusNoContent)
	apiRequest(t, handler, "POST", "/api/play", body, http.StatusConflict)
}
