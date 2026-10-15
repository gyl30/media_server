package main

import (
	"context"
	"encoding/json"
	"encoding/xml"
	"net"
	"net/http"
	"net/http/httptest"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/emiago/sipgo"
	"github.com/emiago/sipgo/sip"
)

func TestCatalogHTTPPreconditions(t *testing.T) {
	const deviceID = "34020000001320000001"
	for _, test := range []struct {
		name, id, error                   string
		create, online, expired, stopping bool
		status                            int
	}{
		{name: "invalid", id: "123", status: http.StatusBadRequest, error: "invalid_request"},
		{name: "unknown", id: deviceID, status: http.StatusNotFound, error: "device_not_found"},
		{name: "offline", id: deviceID, create: true, status: http.StatusConflict, error: "device_offline"},
		{name: "expired", id: deviceID, create: true, online: true, expired: true, status: http.StatusConflict, error: "device_offline"},
		{name: "stopping", id: deviceID, create: true, online: true, stopping: true, status: http.StatusConflict, error: "device_stopping"},
	} {
		t.Run(test.name, func(t *testing.T) {
			s := testInfrastructure(t)
			if test.create {
				if err := s.live.sip.deviceStore.create(t.Context(), gbDevice{deviceID: test.id, name: "camera"}); err != nil {
					t.Fatal(err)
				}
			}
			if test.online {
				expires := time.Now().Add(time.Hour)
				if test.expired {
					expires = time.Now().Add(-time.Second)
				}
				s.live.sip.devices.register(registeredDevice{id: test.id, online: true, expiresAt: expires})
			}
			if test.stopping {
				s.live.mu.Lock()
				s.live.stoppingDevices[test.id] = struct{}{}
				s.live.mu.Unlock()
			}
			response := apiRequest(t, s.handler(), "POST", "/api/devices/"+test.id+"/catalog", "", test.status)
			if !strings.Contains(response.Body.String(), test.error) {
				t.Fatalf("response: %s", response.Body.String())
			}
			if s.live.sip.messageSN.Load() != 0 || len(s.live.sip.channels.pending) != 0 {
				t.Fatal("precondition failure sent or prepared Catalog")
			}
		})
	}
}

func TestCatalogHTTPSIPAcceptance(t *testing.T) {
	const deviceID = "34020000001320000001"
	for _, test := range []struct {
		name                  string
		sipStatus, httpStatus int
		cancel                bool
	}{
		{name: "accepted", sipStatus: sip.StatusOK, httpStatus: http.StatusAccepted},
		{name: "rejected", sipStatus: sip.StatusForbidden, httpStatus: http.StatusBadGateway},
		{name: "timeout", httpStatus: http.StatusGatewayTimeout},
		{name: "cancel", cancel: true, httpStatus: http.StatusBadGateway},
	} {
		t.Run(test.name, func(t *testing.T) {
			s := testInfrastructure(t)
			listener, err := net.ListenPacket("udp", "127.0.0.1:0")
			if err != nil {
				t.Fatal(err)
			}
			ua, err := sipgo.NewUA()
			if err != nil {
				_ = listener.Close()
				t.Fatal(err)
			}
			server, err := sipgo.NewServer(ua)
			if err != nil {
				_ = listener.Close()
				_ = ua.Close()
				t.Fatal(err)
			}
			client, err := sipgo.NewClient(s.live.sip.ua, sipgo.WithClientAddr("127.0.0.1:0"), sipgo.WithClientConnectionAddr("127.0.0.1:0"))
			if err != nil {
				_ = listener.Close()
				_ = ua.Close()
				t.Fatal(err)
			}
			s.live.sip.client = client
			ctx, cancel := context.WithTimeout(t.Context(), 300*time.Millisecond)
			defer cancel()
			queries := make(chan []byte, 1)
			server.OnMessage(func(request *sip.Request, transaction sip.ServerTransaction) {
				queries <- append([]byte(nil), request.Body()...)
				if test.cancel {
					cancel()
				}
				if test.sipStatus != 0 {
					if err := transaction.Respond(sip.NewResponseFromRequest(request, test.sipStatus, "Test result", nil)); err != nil {
						t.Error(err)
					}
				}
			})
			done := make(chan error, 1)
			go func() { done <- server.ServeUDP(listener) }()
			t.Cleanup(func() { _ = listener.Close(); <-done; _ = ua.Close() })
			host, portText, _ := net.SplitHostPort(listener.LocalAddr().String())
			port, _ := strconv.Atoi(portText)
			if err := s.live.sip.deviceStore.create(t.Context(), gbDevice{deviceID: deviceID, name: "camera"}); err != nil {
				t.Fatal(err)
			}
			s.live.sip.devices.register(registeredDevice{
				id: deviceID, contact: sip.Uri{Scheme: "sip", User: deviceID, Host: host, Port: port},
				remoteEndpoint: listener.LocalAddr().String(), online: true, expiresAt: time.Now().Add(time.Hour),
			})
			request := httptest.NewRequest("POST", "/api/devices/"+deviceID+"/catalog", nil).WithContext(ctx)
			response := httptest.NewRecorder()
			s.handler().ServeHTTP(response, request)
			if response.Code != test.httpStatus {
				t.Fatalf("HTTP %d want %d: %s", response.Code, test.httpStatus, response.Body.String())
			}
			var query manscdpQuery
			select {
			case body := <-queries:
				if err := xml.Unmarshal(body, &query); err != nil {
					t.Fatal(err)
				}
			case <-time.After(time.Second):
				t.Fatal("no actual SIP Catalog MESSAGE")
			}
			if query.CmdType != "Catalog" || query.DeviceID != deviceID || query.SN <= 0 {
				t.Fatalf("query: %+v", query)
			}
			s.live.sip.channels.mu.RLock()
			_, pending := s.live.sip.channels.pending[catalogKey{deviceID: deviceID, sn: query.SN}]
			s.live.sip.channels.mu.RUnlock()
			if pending != (test.httpStatus == http.StatusAccepted) {
				t.Fatalf("pending=%t response: %s", pending, response.Body.String())
			}
			if test.httpStatus == http.StatusAccepted {
				var result struct {
					DeviceID string `json:"device_id"`
					SN       int    `json:"sn"`
					Status   string `json:"status"`
				}
				if err := json.Unmarshal(response.Body.Bytes(), &result); err != nil {
					t.Fatal(err)
				}
				if result.DeviceID != deviceID || result.SN != query.SN || result.Status != "accepted" {
					t.Fatalf("acceptance: %+v", result)
				}
				if len(s.live.sip.channels.list(deviceID)) != 0 {
					t.Fatal("SIP acceptance incorrectly claims directory is ready")
				}
			}
		})
	}
}
