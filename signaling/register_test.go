package main

import (
	"context"
	"net"
	"net/http"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/emiago/sipgo"
	"github.com/emiago/sipgo/sip"
)

func TestRegisterAllowlistAndUnregister(t *testing.T) {
	s := testInfrastructure(t)
	listener, err := net.ListenPacket("udp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	done := make(chan error, 1)
	go func() { done <- s.live.sip.server.ServeUDP(listener) }()
	t.Cleanup(func() { _ = listener.Close(); <-done })
	ua, err := sipgo.NewUA()
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = ua.Close() })
	client, err := sipgo.NewClient(ua)
	if err != nil {
		t.Fatal(err)
	}
	deviceID := "34020000001320000001"
	host, portText, _ := net.SplitHostPort(listener.LocalAddr().String())
	port, _ := strconv.Atoi(portText)
	register := func(expires uint32, password string, expected int) {
		t.Helper()
		request := sip.NewRequest(sip.REGISTER, sip.Uri{Scheme: "sip", Host: host, Port: port})
		params := sip.NewParams()
		params.Add("tag", sip.GenerateTagN(16))
		identity := sip.Uri{Scheme: "sip", User: deviceID, Host: s.live.sip.cfg.sipDomain}
		request.AppendHeader(&sip.FromHeader{Address: identity, Params: params})
		request.AppendHeader(&sip.ToHeader{Address: identity})
		request.AppendHeader(&sip.ContactHeader{Address: sip.Uri{Scheme: "sip", User: deviceID, Host: "127.0.0.1", Port: 5062}})
		expiry := sip.ExpiresHeader(expires)
		request.AppendHeader(&expiry)
		request.SetTransport("UDP")
		request.SetDestination(listener.LocalAddr().String())
		ctx, cancel := context.WithTimeout(t.Context(), 2*time.Second)
		defer cancel()
		response, err := client.Do(ctx, request)
		if err != nil {
			t.Fatal(err)
		}
		if password != "" {
			if response.StatusCode != sip.StatusUnauthorized {
				t.Fatalf("challenge: %d", response.StatusCode)
			}
			response, err = client.DoDigestAuth(ctx, request, response, sipgo.DigestAuth{Username: deviceID, Password: password})
			if err != nil {
				t.Fatal(err)
			}
		}
		if response.StatusCode != expected {
			t.Fatalf("REGISTER: %d want %d", response.StatusCode, expected)
		}
	}
	register(120, "", sip.StatusForbidden)
	handler := s.handler()
	apiRequest(t, handler, "POST", "/api/devices", `{"device_id":"34020000001320000001","name":"camera"}`, http.StatusCreated)
	register(120, "wrong-password", sip.StatusUnauthorized)
	register(120, s.live.sip.cfg.sipPassword, sip.StatusOK)
	response := apiRequest(t, handler, "GET", "/api/devices/"+deviceID, "", http.StatusOK)
	if !strings.Contains(response.Body.String(), `"online":true`) {
		t.Fatalf("not online: %s", response.Body.String())
	}
	register(0, s.live.sip.cfg.sipPassword, sip.StatusOK)
	response = apiRequest(t, handler, "GET", "/api/devices/"+deviceID, "", http.StatusOK)
	if !strings.Contains(response.Body.String(), `"online":false`) {
		t.Fatalf("not retained offline: %s", response.Body.String())
	}
}
