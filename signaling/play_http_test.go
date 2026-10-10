package main

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync/atomic"
	"testing"
	"time"
)

func requestPlay(t *testing.T, handler http.Handler) playbackURLs {
	t.Helper()
	response := apiRequest(t, handler, "POST", "/api/devices/34020000001320000001/channels/34020000001320000002/play", "", http.StatusCreated)
	var result playbackURLs
	if err := json.Unmarshal(response.Body.Bytes(), &result); err != nil {
		t.Fatal(err)
	}
	return result
}

func verifyToken(t *testing.T, s *infrastructureServer, value, operation, streamID string, status int) {
	t.Helper()
	body, err := json.Marshal(map[string]string{"token": value, "operation": operation, "stream_id": streamID})
	if err != nil {
		t.Fatal(err)
	}
	request := httptest.NewRequest("POST", "/internal/verify", strings.NewReader(string(body)))
	request.RemoteAddr = "127.0.0.1:12345"
	request.Header.Set("Content-Type", "application/json")
	if s.media.server.controlToken != "" {
		request.Header.Set("Authorization", "Bearer "+s.media.server.controlToken)
	}
	response := httptest.NewRecorder()
	s.handler().ServeHTTP(response, request)
	if response.Code != status {
		t.Fatalf("verify %s %s: HTTP %d want %d: %s", operation, streamID, response.Code, status, response.Body.String())
	}
}

func TestChannelPlayTokensShareLiveAndAreSingleUse(t *testing.T) {
	s := testInfrastructure(t)
	var creates atomic.Int32
	media := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
		if request.URL.Path == "/gb28181/receiver/create" {
			creates.Add(1)
			writeJSON(writer, http.StatusCreated, map[string]int{"rtp_port": 30000})
		} else {
			writer.WriteHeader(http.StatusNoContent)
		}
	}))
	t.Cleanup(media.Close)
	s.media.server.controlURL = media.URL
	invites := testLivePeer(t, s, "34020000001320000001", "34020000001320000002")
	var streamID string
	values := make(map[string]bool)
	for range 3 {
		before := time.Now()
		result := requestPlay(t, s.handler())
		if streamID == "" {
			streamID = result.StreamID
		}
		if result.StreamID != streamID || !validStreamToken(result.Token) || values[result.Token] {
			t.Fatalf("unexpected play identity: %+v", result)
		}
		if result.ExpiresAt.Before(before.Add(s.tokenTTL)) || result.ExpiresAt.After(time.Now().Add(s.tokenTTL)) {
			t.Fatalf("unexpected expiry: %+v", result)
		}
		values[result.Token] = true
		verifyToken(t, s, result.Token, "play", result.StreamID, http.StatusOK)
		verifyToken(t, s, result.Token, "play", result.StreamID, http.StatusForbidden)
	}
	if creates.Load() != 1 || invites.Load() != 1 {
		t.Fatalf("receivers=%d INVITEs=%d", creates.Load(), invites.Load())
	}
	apiRequest(t, s.handler(), "DELETE", "/api/lives/"+streamID, "", http.StatusNoContent)
}

func TestPlayExpiryWrongSourceAndStop(t *testing.T) {
	s := testInfrastructure(t)
	streamID := "11111111-1111-4111-8111-111111111111"
	session := testStreamingLive(s, "34020000001320000002", streamID)
	result, err := s.newPlaybackURLs(playSource{LiveID: streamID})
	if err != nil {
		t.Fatal(err)
	}
	verifyToken(t, s, result.Token, "publish", streamID, http.StatusForbidden)
	verifyToken(t, s, result.Token, "play", "wrong-source", http.StatusForbidden)
	verifyToken(t, s, result.Token, "play", streamID, http.StatusOK)
	expired, err := s.newPlaybackURLs(playSource{LiveID: streamID})
	if err != nil {
		t.Fatal(err)
	}
	s.live.sip.now = func() time.Time { return expired.ExpiresAt }
	verifyToken(t, s, expired.Token, "play", streamID, http.StatusForbidden)
	s.tokens.expire(expired.ExpiresAt)
	s.live.sip.now = time.Now
	pending, err := s.newPlaybackURLs(playSource{LiveID: streamID})
	if err != nil {
		t.Fatal(err)
	}
	s.live.remove(session)
	verifyToken(t, s, pending.Token, "play", streamID, http.StatusForbidden)
	apiRequest(t, s.handler(), "POST", "/play/whep/retired-ticket", "offer", http.StatusNotFound)
}
