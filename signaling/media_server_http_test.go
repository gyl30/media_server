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
			StreamID string `json:"stream_id"`
			URL      string `json:"url"`
			Username string `json:"username"`
			Password string `json:"password"`
		}
		decoder := json.NewDecoder(request.Body)
		decoder.DisallowUnknownFields()
		if err := decoder.Decode(&command); err != nil {
			writeHTTPError(writer, http.StatusBadRequest, "invalid_request")
			return
		}
		if command.StreamID != "8cbb0faf-3a7e-49bc-a64e-343dfb334954" || command.URL != "rtsp://camera.example/live" {
			t.Errorf("unexpected RTSP create identity: %+v", command)
		}
		writer.WriteHeader(http.StatusCreated)
	}))
	t.Cleanup(media.Close)
	client := newMediaServerHTTPClient(mediaServer{controlURL: media.URL}, time.Second)
	if err := client.createRTSPPull(t.Context(), rtspPullCreateRequest{
		StreamID: "8cbb0faf-3a7e-49bc-a64e-343dfb334954", URL: "rtsp://camera.example/live",
	}); err != nil {
		t.Fatalf("media rejected signaling RTSP request: %v", err)
	}
}

func TestMediaControlRequestsCarryToken(t *testing.T) {
	var authorizations []string
	media := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
		authorizations = append(authorizations, request.Header.Get("Authorization"))
		if request.URL.Path == "/receivers" {
			writer.Header().Set("Content-Type", "application/json")
			_, _ = writer.Write([]byte(`{"receivers":[]}`))
			return
		}
		if request.Method != http.MethodPost || request.URL.Path != "/receivers/delete" {
			t.Errorf("unexpected delete request: %s %s", request.Method, request.URL.Path)
		}
		var command struct {
			StreamID string `json:"stream_id"`
		}
		decoder := json.NewDecoder(request.Body)
		decoder.DisallowUnknownFields()
		if err := decoder.Decode(&command); err != nil || command.StreamID != "id" {
			t.Errorf("invalid receiver delete identity: %+v, %v", command, err)
		}
		writer.WriteHeader(http.StatusNoContent)
	}))
	t.Cleanup(media.Close)
	client := newMediaServerHTTPClient(mediaServer{controlURL: media.URL, controlToken: "secret"}, time.Second)
	if err := client.deleteReceiver(t.Context(), "id"); err != nil {
		t.Fatal(err)
	}
	if _, err := client.listReceivers(t.Context()); err != nil {
		t.Fatal(err)
	}
	if len(authorizations) != 2 || authorizations[0] != "Bearer secret" || authorizations[1] != "Bearer secret" {
		t.Fatalf("control requests missing token: %v", authorizations)
	}
}

func TestGBReceiverRequestsUseOnlyStreamIdentity(t *testing.T) {
	var creates, updates int
	media := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
		if request.Method != http.MethodPost {
			t.Errorf("unexpected media method: %s", request.Method)
		}
		decoder := json.NewDecoder(request.Body)
		decoder.DisallowUnknownFields()
		switch request.URL.Path {
		case "/gb28181/receiver/create":
			var command struct {
				StreamID    string `json:"stream_id"`
				Transport   string `json:"transport"`
				PayloadType uint8  `json:"payload_type"`
				SSRC        uint32 `json:"ssrc"`
			}
			if err := decoder.Decode(&command); err != nil || command.StreamID != "run-id" ||
				command.Transport != "udp" || command.PayloadType != 96 || command.SSRC != 123 {
				t.Errorf("invalid receiver create: %+v, %v", command, err)
			}
			creates++
			writeJSON(writer, http.StatusCreated, map[string]uint16{"rtp_port": 30000})
		case "/gb28181/receiver/update":
			var command struct {
				StreamID string `json:"stream_id"`
				SSRC     uint32 `json:"ssrc"`
			}
			if err := decoder.Decode(&command); err != nil || command.StreamID != "run-id" || command.SSRC != 456 {
				t.Errorf("invalid receiver update: %+v, %v", command, err)
			}
			updates++
			writer.WriteHeader(http.StatusNoContent)
		default:
			t.Errorf("unexpected media path: %s", request.URL.Path)
			writer.WriteHeader(http.StatusNotFound)
		}
	}))
	t.Cleanup(media.Close)
	client := newMediaServerHTTPClient(mediaServer{controlURL: media.URL}, time.Second)
	port, err := client.createUDPReceiver(t.Context(), gb28181ReceiverRequest{streamID: "run-id", payloadType: 96, ssrc: 123})
	if err != nil || port != 30000 {
		t.Fatalf("receiver create port=%d err=%v", port, err)
	}
	if err := client.updateReceiverSSRC(t.Context(), "run-id", 456); err != nil {
		t.Fatal(err)
	}
	if creates != 1 || updates != 1 {
		t.Fatalf("create=%d update=%d", creates, updates)
	}
}
