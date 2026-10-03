package main

import (
	"net"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"
)

func TestDeviceDeleteFencesPlayUntilReceiverRemoved(t *testing.T) {
	s := testInfrastructure(t)
	deleting, release := make(chan struct{}), make(chan struct{})
	var releaseOnce sync.Once
	defer releaseOnce.Do(func() { close(release) })
	var creates, removes atomic.Int32
	media := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
		if request.URL.Path == "/gb28181/receiver/create" {
			creates.Add(1)
			writeJSON(writer, http.StatusCreated, map[string]int{"rtp_port": 30000})
		} else {
			if removes.Add(1) == 1 {
				close(deleting)
				select {
				case <-release:
				case <-request.Context().Done():
					return
				}
			}
			writer.WriteHeader(http.StatusNoContent)
		}
	}))
	t.Cleanup(media.Close)
	s.media.server.controlURL = media.URL
	testLivePeer(t, s, "34020000001320000001", "34020000001320000002")
	testLivePeer(t, s, "34020000001320000003", "34020000001320000004")
	handler := s.handler()
	pending := requestPlay(t, handler)
	deleted := make(chan *httptest.ResponseRecorder, 1)
	go func() {
		response := httptest.NewRecorder()
		handler.ServeHTTP(response, httptest.NewRequest("DELETE", "/api/devices/34020000001320000001", nil))
		deleted <- response
	}()
	select {
	case <-deleting:
	case response := <-deleted:
		t.Fatalf("device delete returned before cleanup: %d %s", response.Code, response.Body.String())
	case <-time.After(2 * time.Second):
		t.Fatal("receiver cleanup not reached")
	}
	apiRequest(t, handler, "POST", "/api/devices/34020000001320000001/channels/34020000001320000002/play", "", http.StatusConflict)
	apiRequest(t, handler, "POST", pending.WHEPURL, "offer", http.StatusNotFound)
	if creates.Load() != 1 {
		t.Fatal("new receiver created during device deletion")
	}
	apiRequest(t, handler, "POST", "/api/devices/34020000001320000003/channels/34020000001320000004/play", "", http.StatusCreated)
	releaseOnce.Do(func() { close(release) })
	response := <-deleted
	if response.Code != http.StatusNoContent {
		t.Fatalf("DELETE: %d %s", response.Code, response.Body.String())
	}
	apiRequest(t, handler, "GET", "/api/devices/34020000001320000001", "", http.StatusNotFound)
	apiRequest(t, handler, "DELETE", "/api/lives/"+pending.LiveID, "", http.StatusNotFound)
	apiRequest(t, handler, "POST", "/api/devices/34020000001320000001/channels/34020000001320000002/play", "", http.StatusNotFound)
	response = apiRequest(t, handler, "GET", "/api/devices", "", http.StatusOK)
	if strings.Contains(response.Body.String(), "34020000001320000001") || !strings.Contains(response.Body.String(), "34020000001320000003") {
		t.Fatal(response.Body.String())
	}
}

func TestDeviceExpiryStopsLiveButRetainsConfiguration(t *testing.T) {
	for _, expiry := range []string{"heartbeat", "registration"} {
		t.Run(expiry, func(t *testing.T) {
			s := testInfrastructure(t)
			var deletes atomic.Int32
			media := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
				if request.URL.Path == "/gb28181/receiver/create" {
					writeJSON(writer, http.StatusCreated, map[string]int{"rtp_port": 30000})
				} else {
					deletes.Add(1)
					writer.WriteHeader(http.StatusNoContent)
				}
			}))
			t.Cleanup(media.Close)
			s.media.server.controlURL = media.URL
			testLivePeer(t, s, "34020000001320000001", "34020000001320000002")
			handler := s.handler()
			ticket := requestPlay(t, handler)
			delta := s.live.sip.cfg.heartbeatTimeout + time.Second
			if expiry == "registration" {
				delta = time.Hour + time.Second
			}
			s.live.sip.expireDevices(time.Now().Add(delta))
			device := apiRequest(t, handler, "GET", "/api/devices/34020000001320000001", "", http.StatusOK)
			if !strings.Contains(device.Body.String(), `"online":false`) {
				t.Fatal(device.Body.String())
			}
			channels := apiRequest(t, handler, "GET", "/api/devices/34020000001320000001/channels", "", http.StatusOK)
			if strings.TrimSpace(channels.Body.String()) != `{"channels":[]}` {
				t.Fatal(channels.Body.String())
			}
			apiRequest(t, handler, "POST", ticket.WHEPURL, "offer", http.StatusNotFound)
			apiRequest(t, handler, "DELETE", "/api/lives/"+ticket.LiveID, "", http.StatusNotFound)
			if deletes.Load() != 1 {
				t.Fatalf("receiver deletes: %d", deletes.Load())
			}
		})
	}
}

func TestUnavailableMediaDoesNotRetainLive(t *testing.T) {
	s := testInfrastructure(t)
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	s.media.server.controlURL = "http://" + listener.Addr().String()
	_ = listener.Close()
	peer := testLivePeer(t, s, "34020000001320000001", "34020000001320000002")
	handler := s.handler()
	apiRequest(t, handler, "POST", "/api/devices/34020000001320000001/channels/34020000001320000002/play", "", http.StatusBadGateway)
	channels := apiRequest(t, handler, "GET", "/api/devices/34020000001320000001/channels", "", http.StatusOK)
	if strings.Contains(channels.Body.String(), `"live"`) {
		t.Fatalf("unavailable media retained live: %s", channels.Body.String())
	}
	if peer.Load() != 0 {
		t.Fatal("INVITE sent without receiver")
	}
	s.live.ssrcs.mu.Lock()
	active := len(s.live.ssrcs.active)
	s.live.ssrcs.mu.Unlock()
	if active != 0 {
		t.Fatalf("SSRCs retained: %d", active)
	}
}

func TestDeviceDeleteRetainsConfigurationUntilMediaCleanupConfirmed(t *testing.T) {
	s := testInfrastructure(t)
	var deleteStatus atomic.Int32
	deleteStatus.Store(http.StatusServiceUnavailable)
	media := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
		if request.URL.Path == "/gb28181/receiver/create" {
			writeJSON(writer, http.StatusCreated, map[string]int{"rtp_port": 30000})
		} else {
			writeHTTPError(writer, int(deleteStatus.Load()), "receiver_cleanup")
		}
	}))
	t.Cleanup(media.Close)
	s.media.server.controlURL = media.URL
	testLivePeer(t, s, "34020000001320000001", "34020000001320000002")
	handler := s.handler()
	ticket := requestPlay(t, handler)
	apiRequest(t, handler, "DELETE", "/api/devices/34020000001320000001", "", http.StatusBadGateway)
	apiRequest(t, handler, "GET", "/api/devices/34020000001320000001", "", http.StatusOK)
	apiRequest(t, handler, "POST", ticket.WHEPURL, "offer", http.StatusNotFound)
	apiRequest(t, handler, "POST", "/api/devices/34020000001320000001/channels/34020000001320000002/play", "", http.StatusConflict)
	deleteStatus.Store(http.StatusNotFound)
	apiRequest(t, handler, "DELETE", "/api/devices/34020000001320000001", "", http.StatusNoContent)
	apiRequest(t, handler, "GET", "/api/devices/34020000001320000001", "", http.StatusNotFound)
}

func TestDeviceDeleteContinuesAfterRemoteByeFailure(t *testing.T) {
	s := testInfrastructure(t)
	var deletes atomic.Int32
	media := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
		if request.URL.Path == "/gb28181/receiver/create" {
			writeJSON(writer, http.StatusCreated, map[string]int{"rtp_port": 30000})
		} else {
			deletes.Add(1)
			writer.WriteHeader(http.StatusNoContent)
		}
	}))
	t.Cleanup(media.Close)
	s.media.server.controlURL = media.URL
	testLivePeer(t, s, "34020000001320000001", "34020000001320000002", http.StatusServiceUnavailable)
	handler := s.handler()
	ticket := requestPlay(t, handler)
	apiRequest(t, handler, "DELETE", "/api/devices/34020000001320000001", "", http.StatusNoContent)
	apiRequest(t, handler, "GET", "/api/devices/34020000001320000001", "", http.StatusNotFound)
	apiRequest(t, handler, "DELETE", "/api/lives/"+ticket.LiveID, "", http.StatusNotFound)
	if deletes.Load() != 1 {
		t.Fatalf("media deletes: %d", deletes.Load())
	}
	s.live.ssrcs.mu.Lock()
	active := len(s.live.ssrcs.active)
	s.live.ssrcs.mu.Unlock()
	if active != 0 {
		t.Fatalf("SSRCs retained: %d", active)
	}
}
