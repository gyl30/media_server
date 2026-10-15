package main

import (
	"context"
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"
)

func TestGBReceiverKindsHTTPContract(t *testing.T) {
	for _, test := range []struct {
		name        string
		kind        receiverKind
		response    string
		status      int
		contentType string
		want        uint16
	}{
		{"live", liveReceiver, `{"rtp_port":30000}`, 201, "application/json", 30000},
		{"playback", playbackReceiver, `{"rtp_port":30001}`, 201, "application/json", 30001},
		{"live_zero", liveReceiver, `{"rtp_port":0}`, 201, "application/json", 0},
		{"live_odd", liveReceiver, `{"rtp_port":30001}`, 201, "application/json", 0},
		{"playback_zero", playbackReceiver, `{"rtp_port":0}`, 201, "application/json", 0},
		{"live_wrong_field", liveReceiver, `{"listen_port":30000}`, 201, "application/json", 0},
		{"playback_listen_port_rejected", playbackReceiver, `{"listen_port":30000}`, 201, "application/json", 0},
		{"missing", playbackReceiver, `{}`, 201, "application/json", 0},
		{"extra_field", playbackReceiver, `{"rtp_port":30000,"extra":1}`, 201, "application/json", 0},
		{"both_fields", playbackReceiver, `{"listen_port":30000,"rtp_port":30000}`, 201, "application/json", 0},
		{"null", playbackReceiver, `{"rtp_port":null}`, 201, "application/json", 0},
		{"negative", playbackReceiver, `{"rtp_port":-1}`, 201, "application/json", 0},
		{"overflow", playbackReceiver, `{"rtp_port":65536}`, 201, "application/json", 0},
		{"string", playbackReceiver, `{"rtp_port":"30000"}`, 201, "application/json", 0},
		{"broken_json", playbackReceiver, `{"rtp_port":`, 201, "application/json", 0},
		{"trailing_json", playbackReceiver, `{"rtp_port":30000}{}`, 201, "application/json", 0},
		{"wrong_type", playbackReceiver, `{"rtp_port":30000}`, 201, "text/plain", 0},
		{"unauthorized", playbackReceiver, `{"error":"unauthorized"}`, 403, "application/json", 0},
		{"rejected", playbackReceiver, `{"error":"port_unavailable"}`, 503, "application/json", 0},
	} {
		t.Run(test.name, func(t *testing.T) {
			requests := 0
			server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				requests++
				path, transport := "/gb28181/receiver/create", "udp"
				if test.kind == playbackReceiver {
					path, transport = "/gb28181/receiver/playback/create", "tcp_passive"
				}
				if r.Method != http.MethodPost || r.URL.Path != path || r.Header.Get("Authorization") != "Bearer fixture-control" || r.Header.Get("Content-Type") != "application/json" {
					t.Errorf("invalid control request: %s %s %v", r.Method, r.URL.Path, r.Header)
				}
				var body map[string]json.RawMessage
				if err := json.NewDecoder(r.Body).Decode(&body); err != nil || len(body) != 4 || string(body["stream_id"]) != `"fixture-id"` || string(body["transport"]) != `"`+transport+`"` || string(body["payload_type"]) != "96" || string(body["ssrc"]) != "1200000001" {
					t.Errorf("unexpected receiver request: %s, %v", body, err)
				}
				w.Header().Set("Content-Type", test.contentType)
				w.WriteHeader(test.status)
				_, _ = io.WriteString(w, test.response)
			}))
			defer server.Close()
			client := newMediaServerHTTPClient(mediaServer{controlURL: server.URL, controlToken: "fixture-control"}, time.Second)
			port, err := client.createReceiver(t.Context(), gb28181ReceiverRequest{streamID: "fixture-id", payloadType: 96, ssrc: 1200000001}, test.kind)
			if requests != 1 || port != test.want || (err == nil) != (test.want != 0) {
				t.Fatalf("requests=%d port=%d want=%d err=%v", requests, port, test.want, err)
			}
			if test.status != 201 {
				var rejection *mediaServerHTTPRejection
				if !errors.As(err, &rejection) || rejection.status != test.status {
					t.Fatalf("HTTP rejection lost: %v", err)
				}
			}
		})
	}
}

func TestGBReceiverCreationHonorsCanceledContext(t *testing.T) {
	for _, kind := range []receiverKind{liveReceiver, playbackReceiver} {
		ctx, cancel := context.WithCancel(t.Context())
		cancel()
		client := newMediaServerHTTPClient(mediaServer{controlURL: "http://127.0.0.1:1"}, time.Second)
		if _, err := client.createReceiver(ctx, gb28181ReceiverRequest{}, kind); !errors.Is(err, context.Canceled) {
			t.Fatalf("cancellation lost: %v", err)
		}
	}
}
