package main

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"
)

func TestRTSPPullCreateMatchesMediaContract(t *testing.T) {
	media := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
		if request.Method != http.MethodPost || request.URL.Path != "/rtsp/pull/create" {
			t.Errorf("unexpected media request: %s %s", request.Method, request.URL.Path)
		}
		var command struct {
			StreamID   string `json:"stream_id"`
			StreamName string `json:"stream_name"`
			URL        string `json:"url"`
			Username   string `json:"username"`
			Password   string `json:"password"`
		}
		decoder := json.NewDecoder(request.Body)
		decoder.DisallowUnknownFields()
		if err := decoder.Decode(&command); err != nil {
			writeHTTPError(writer, http.StatusBadRequest, "invalid_request")
			return
		}
		if command.StreamID != "8cbb0faf-3a7e-49bc-a64e-343dfb334954" || command.StreamName != "rtsp/camera" || command.URL != "rtsp://camera.example/live" {
			t.Errorf("unexpected RTSP create identity: %+v", command)
		}
		writer.WriteHeader(http.StatusCreated)
	}))
	t.Cleanup(media.Close)
	client := newMediaServerHTTPClient(mediaServer{controlURL: media.URL}, time.Second)
	if err := client.createRTSPPull(t.Context(), rtspPullCreateRequest{
		StreamID: "8cbb0faf-3a7e-49bc-a64e-343dfb334954", StreamName: "rtsp/camera", URL: "rtsp://camera.example/live",
	}); err != nil {
		t.Fatalf("media rejected signaling RTSP request: %v", err)
	}
}
