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

type testPlayResponse struct {
	LiveID    string    `json:"live_id"`
	PlayID    string    `json:"play_id"`
	ExpiresAt time.Time `json:"expires_at"`
	WHEPURL   string    `json:"whep_url"`
}

func requestPlay(t *testing.T, handler http.Handler) testPlayResponse {
	t.Helper()
	response := apiRequest(t, handler, "POST", "/api/devices/34020000001320000001/channels/34020000001320000002/play", "", http.StatusCreated)
	var ticket testPlayResponse
	if err := json.Unmarshal(response.Body.Bytes(), &ticket); err != nil {
		t.Fatal(err)
	}
	return ticket
}

func TestChannelPlayTicketsAreIndependentAndSingleUse(t *testing.T) {
	s := testInfrastructure(t)
	var creates, offers atomic.Int32
	var streamID atomic.Value
	streamID.Store("")
	media := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
		switch request.URL.Path {
		case "/gb28181/receiver/create":
			var command struct {
				StreamID string `json:"stream_id"`
			}
			if err := json.NewDecoder(request.Body).Decode(&command); err != nil {
				t.Errorf("invalid receiver create body: %v", err)
			}
			streamID.Store(command.StreamID)
			creates.Add(1)
			writeJSON(writer, http.StatusCreated, map[string]int{"rtp_port": 30000})
		case "/play/whep/" + streamID.Load().(string):
			offers.Add(1)
			writer.Header().Set("Content-Type", "application/sdp")
			writer.Header().Set("Location", "/play/whep/session/viewer")
			writer.Header().Set("Cache-Control", "no-store")
			writer.Header().Set("Access-Control-Allow-Origin", "*")
			writer.Header().Set("Access-Control-Expose-Headers", "Location")
			writer.WriteHeader(http.StatusCreated)
			_, _ = writer.Write([]byte("answer"))
		default:
			writer.WriteHeader(http.StatusNoContent)
		}
	}))
	t.Cleanup(media.Close)
	s.media.server.controlURL = media.URL
	s.media.server.mediaIP = "192.0.2.10"
	s.media.server.httpPort = 8081
	invites := testLivePeer(t, s, "34020000001320000001", "34020000001320000002")
	handler := s.handler()
	var previousID, liveID string
	for range 3 {
		before := time.Now()
		ticket := requestPlay(t, handler)
		if liveID == "" {
			liveID = ticket.LiveID
		}
		if ticket.LiveID != liveID || !validUUIDv4(ticket.PlayID) || ticket.PlayID == previousID {
			t.Fatalf("identities: %+v", ticket)
		}
		if ticket.ExpiresAt.Before(before.Add(30*time.Second)) || ticket.ExpiresAt.After(time.Now().Add(30*time.Second)) {
			t.Fatalf("expiry: %+v", ticket)
		}
		previousID = ticket.PlayID
		options := httptest.NewRecorder()
		handler.ServeHTTP(options, httptest.NewRequest("OPTIONS", ticket.WHEPURL, nil))
		if options.Code != 200 || options.Header().Get("Access-Control-Allow-Origin") != "*" {
			t.Fatalf("OPTIONS: %+v", options)
		}
		responses := make(chan *httptest.ResponseRecorder, 2)
		for range 2 {
			go func() {
				request := httptest.NewRequest("POST", ticket.WHEPURL, strings.NewReader("offer"))
				request.Header.Set("Content-Type", "application/sdp")
				response := httptest.NewRecorder()
				handler.ServeHTTP(response, request)
				responses <- response
			}()
		}
		var success, rejected int
		for range 2 {
			response := <-responses
			switch response.Code {
			case http.StatusCreated:
				success++
				if response.Header().Get("Location") != "http://192.0.2.10:8081/play/whep/session/viewer" || response.Body.String() != "answer" || response.Header().Get("Access-Control-Expose-Headers") != "Location" {
					t.Fatalf("proxy: %+v", response)
				}
			case http.StatusNotFound:
				rejected++
			default:
				t.Fatalf("consume HTTP %d: %s", response.Code, response.Body.String())
			}
		}
		if success != 1 || rejected != 1 {
			t.Fatalf("success=%d rejected=%d", success, rejected)
		}
	}
	if creates.Load() != 1 || invites.Load() != 1 || offers.Load() != 3 {
		t.Fatalf("receivers=%d INVITEs=%d offers=%d", creates.Load(), invites.Load(), offers.Load())
	}
	results := make(chan *httptest.ResponseRecorder, 8)
	for range 8 {
		go func() {
			response := httptest.NewRecorder()
			handler.ServeHTTP(response, httptest.NewRequest("POST", "/api/devices/34020000001320000001/channels/34020000001320000002/play", nil))
			results <- response
		}()
	}
	ids := make(map[string]bool)
	for range 8 {
		response := <-results
		var ticket testPlayResponse
		if err := json.Unmarshal(response.Body.Bytes(), &ticket); err != nil {
			t.Fatal(err)
		}
		if response.Code != http.StatusCreated || ticket.LiveID != liveID || ids[ticket.PlayID] {
			t.Fatalf("concurrent play: %d %+v", response.Code, ticket)
		}
		ids[ticket.PlayID] = true
	}
	if creates.Load() != 1 || invites.Load() != 1 {
		t.Fatal("concurrent play created another upstream")
	}
}

func TestLiveStopWaitsForConsumedOffer(t *testing.T) {
	s := testInfrastructure(t)
	entered, release := make(chan struct{}), make(chan struct{})
	var deletes atomic.Int32
	media := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
		switch request.URL.Path {
		case "/gb28181/receiver/create":
			writeJSON(writer, http.StatusCreated, map[string]int{"rtp_port": 30000})
		case "/receivers/delete":
			deletes.Add(1)
			writer.WriteHeader(http.StatusNoContent)
		default:
			close(entered)
			select {
			case <-release:
			case <-request.Context().Done():
				return
			}
			writer.Header().Set("Location", "/play/whep/session/old-viewer")
			writer.WriteHeader(http.StatusCreated)
		}
	}))
	t.Cleanup(media.Close)
	s.media.server.controlURL = media.URL
	testLivePeer(t, s, "34020000001320000001", "34020000001320000002")
	handler := s.handler()
	ticket := requestPlay(t, handler)
	offerDone := make(chan int, 1)
	go func() {
		response := httptest.NewRecorder()
		request := httptest.NewRequest("POST", ticket.WHEPURL, strings.NewReader("offer"))
		request.Header.Set("Content-Type", "application/sdp")
		handler.ServeHTTP(response, request)
		offerDone <- response.Code
	}()
	select {
	case <-entered:
	case <-time.After(2 * time.Second):
		t.Fatal("offer not forwarded")
	}
	stopDone := make(chan int, 1)
	go func() {
		response := httptest.NewRecorder()
		handler.ServeHTTP(response, httptest.NewRequest("DELETE", "/api/lives/"+ticket.LiveID, nil))
		stopDone <- response.Code
	}()
	deadline := time.Now().Add(time.Second)
	for {
		channels := apiRequest(t, handler, "GET", "/api/devices/34020000001320000001/channels", "", http.StatusOK)
		if strings.Contains(channels.Body.String(), `"state":"stopping"`) {
			break
		}
		if time.Now().After(deadline) {
			t.Fatal("live did not enter stopping")
		}
		time.Sleep(time.Millisecond)
	}
	if deletes.Load() != 0 {
		t.Fatal("receiver deleted during outstanding offer")
	}
	apiRequest(t, handler, "POST", "/api/devices/34020000001320000001/channels/34020000001320000002/play", "", http.StatusConflict)
	close(release)
	if code := <-offerDone; code != http.StatusCreated {
		t.Fatalf("offer HTTP %d", code)
	}
	if code := <-stopDone; code != http.StatusNoContent {
		t.Fatalf("stop HTTP %d", code)
	}
	if deletes.Load() != 1 {
		t.Fatalf("deletes=%d", deletes.Load())
	}
}

func TestPlayExpiryFailureAndStop(t *testing.T) {
	s := testInfrastructure(t)
	var offers atomic.Int32
	var offerStatus atomic.Int32
	offerStatus.Store(http.StatusBadRequest)
	media := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
		switch request.URL.Path {
		case "/gb28181/receiver/create":
			writeJSON(writer, http.StatusCreated, map[string]int{"rtp_port": 30000})
		case "/receivers/delete":
			writer.WriteHeader(http.StatusNoContent)
		default:
			offers.Add(1)
			writer.WriteHeader(int(offerStatus.Load()))
		}
	}))
	t.Cleanup(media.Close)
	s.media.server.controlURL = media.URL
	invites := testLivePeer(t, s, "34020000001320000001", "34020000001320000002")
	handler := s.handler()
	consume := func(ticket testPlayResponse, body, contentType string, status int) {
		t.Helper()
		request := httptest.NewRequest("POST", ticket.WHEPURL, strings.NewReader(body))
		request.Header.Set("Content-Type", contentType)
		response := httptest.NewRecorder()
		handler.ServeHTTP(response, request)
		if response.Code != status {
			t.Fatalf("consume: HTTP %d want %d: %s", response.Code, status, response.Body.String())
		}
	}
	first := requestPlay(t, handler)
	// Test the exact expiry boundary independently of the periodic sweep.
	s.live.sip.now = func() time.Time { return first.ExpiresAt }
	consume(first, "offer", "application/sdp", http.StatusNotFound)
	s.live.expirePlayTickets(first.ExpiresAt)
	s.live.sip.now = time.Now
	second := requestPlay(t, handler)
	if second.LiveID != first.LiveID || invites.Load() != 1 {
		t.Fatal("ticket expiry stopped live")
	}
	consume(second, "invalid SDP", "application/sdp", http.StatusBadRequest)
	consume(second, "offer", "application/sdp", http.StatusNotFound)
	third := requestPlay(t, handler)
	offerStatus.Store(http.StatusServiceUnavailable)
	consume(third, "offer", "application/sdp", http.StatusServiceUnavailable)
	consume(third, "offer", "application/sdp", http.StatusNotFound)
	fourth := requestPlay(t, handler)
	consume(fourth, "offer", "text/plain", http.StatusUnsupportedMediaType)
	consume(fourth, "offer", "application/sdp", http.StatusNotFound)
	fifth := requestPlay(t, handler)
	consume(fifth, strings.Repeat("x", 1024*1024+1), "application/sdp", http.StatusBadRequest)
	consume(fifth, "offer", "application/sdp", http.StatusNotFound)
	if offers.Load() != 2 {
		t.Fatalf("invalid or consumed offers forwarded: %d", offers.Load())
	}
	pending := requestPlay(t, handler)
	apiRequest(t, handler, "DELETE", "/api/lives/"+first.LiveID, "", http.StatusNoContent)
	consume(pending, "offer", "application/sdp", http.StatusNotFound)
	apiRequest(t, handler, "DELETE", "/api/lives/"+first.LiveID, "", http.StatusNotFound)
	next := requestPlay(t, handler)
	if next.LiveID == first.LiveID {
		t.Fatal("new live reused old generation")
	}
	apiRequest(t, handler, "DELETE", "/api/lives/"+first.LiveID, "", http.StatusNotFound)
	if current := requestPlay(t, handler); current.LiveID != next.LiveID {
		t.Fatal("old delete affected new generation")
	}
	media.Close()
	consume(next, "offer", "application/sdp", http.StatusBadGateway)
	consume(next, "offer", "application/sdp", http.StatusNotFound)
}

func TestPlayPreconditions(t *testing.T) {
	s := testInfrastructure(t)
	handler := s.handler()
	path := "/api/devices/34020000001320000001/channels/34020000001320000002/play"
	response := apiRequest(t, handler, "POST", path, "", http.StatusNotFound)
	if !strings.Contains(response.Body.String(), "device_not_found") {
		t.Fatal(response.Body.String())
	}
	apiRequest(t, handler, "POST", "/api/devices", `{"device_id":"34020000001320000001","name":"camera"}`, http.StatusCreated)
	response = apiRequest(t, handler, "POST", path, "", http.StatusConflict)
	if !strings.Contains(response.Body.String(), "device_offline") {
		t.Fatal(response.Body.String())
	}
	apiRequest(t, handler, "DELETE", "/api/devices/34020000001320000001", "", http.StatusNoContent)
	testLivePeer(t, s, "34020000001320000001", "34020000001320000002")
	response = apiRequest(t, handler, "POST", "/api/devices/34020000001320000001/channels/34020000001320000003/play", "", http.StatusNotFound)
	if !strings.Contains(response.Body.String(), "channel_not_found") {
		t.Fatal(response.Body.String())
	}
	s.live.sip.channels.beginQuery("34020000001320000001", 2)
	catalog := catalogResponse{DeviceID: "34020000001320000001", SN: 2, SumNum: 1}
	catalog.DeviceList.Num = 1
	catalog.DeviceList.Items = []catalogChannel{{DeviceID: "34020000001320000002", Status: "OFF"}}
	if err := s.live.sip.channels.apply(catalog); err != nil {
		t.Fatal(err)
	}
	response = apiRequest(t, handler, "POST", path, "", http.StatusConflict)
	if !strings.Contains(response.Body.String(), "channel_offline") {
		t.Fatal(response.Body.String())
	}
}
