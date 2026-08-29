package main

import (
	"context"
	"fmt"
	"io"
	"log/slog"
	"net"
	"strconv"
	"testing"
	"time"

	"github.com/emiago/sipgo"
	"github.com/emiago/sipgo/sip"
)

const testDeviceID = "34020000001320000001"

func TestRegisterDigestAndRefresh(t *testing.T) {
	cfg := testConfig()
	server, addr := startRegistrar(t, cfg)
	client := newRegisterClient(t)

	request := newRegisterRequest(t, cfg, addr, testDeviceID, 120, "192.0.2.10", 5060)
	challenge := doSIP(t, client, request)
	if challenge.StatusCode != sip.StatusUnauthorized {
		t.Fatalf("initial status = %d", challenge.StatusCode)
	}
	if challenge.GetHeader("WWW-Authenticate") == nil {
		t.Fatal("WWW-Authenticate is missing")
	}
	response := doDigest(t, client, request, challenge, testDeviceID, cfg.sipPassword)
	if response.StatusCode != sip.StatusOK {
		t.Fatalf("authenticated status = %d", response.StatusCode)
	}
	registered, ok := server.devices.get(testDeviceID)
	if !ok || !registered.online {
		t.Fatal("device is not registered online")
	}
	if registered.contact.Host != "192.0.2.10" || registered.remoteEndpoint == "192.0.2.10:5060" {
		t.Fatalf("contact = %s remote = %s", registered.contact.String(), registered.remoteEndpoint)
	}

	refresh := newRegisterRequest(t, cfg, addr, testDeviceID, 240, "192.0.2.11", 5062)
	challenge = doSIP(t, client, refresh)
	response = doDigest(t, client, refresh, challenge, testDeviceID, cfg.sipPassword)
	if response.StatusCode != sip.StatusOK {
		t.Fatalf("refresh status = %d", response.StatusCode)
	}
	registered, ok = server.devices.get(testDeviceID)
	if !ok || registered.contact.Host != "192.0.2.11" || server.devices.len() != 1 {
		t.Fatalf("refresh device = %+v count = %d", registered, server.devices.len())
	}
}

func TestRegisterRejectsMissingOrWrongAuthorization(t *testing.T) {
	cfg := testConfig()
	server, addr := startRegistrar(t, cfg)
	client := newRegisterClient(t)

	request := newRegisterRequest(t, cfg, addr, testDeviceID, 120, "192.0.2.10", 5060)
	challenge := doSIP(t, client, request)
	if challenge.StatusCode != sip.StatusUnauthorized {
		t.Fatalf("missing authorization status = %d", challenge.StatusCode)
	}
	response := doDigest(t, client, request.Clone(), challenge, testDeviceID, "wrong-password")
	if response.StatusCode != sip.StatusUnauthorized {
		t.Fatalf("wrong password status = %d", response.StatusCode)
	}
	if server.devices.len() != 0 {
		t.Fatalf("registered devices = %d", server.devices.len())
	}
	response = doDigest(t, client, request.Clone(), challenge, testDeviceID, cfg.sipPassword)
	if response.StatusCode != sip.StatusOK {
		t.Fatalf("correct password after failed attempt status = %d", response.StatusCode)
	}
}

func TestRegisterExpiresZeroUnregisters(t *testing.T) {
	cfg := testConfig()
	server, addr := startRegistrar(t, cfg)
	client := newRegisterClient(t)
	registerDevice(t, client, cfg, addr, testDeviceID, 120)

	request := newRegisterRequest(t, cfg, addr, testDeviceID, 0, "192.0.2.10", 5060)
	challenge := doSIP(t, client, request)
	response := doDigest(t, client, request, challenge, testDeviceID, cfg.sipPassword)
	if response.StatusCode != sip.StatusOK {
		t.Fatalf("unregister status = %d", response.StatusCode)
	}
	if _, ok := server.devices.get(testDeviceID); ok {
		t.Fatal("device remains registered")
	}
}

func TestRegisterRejectsMalformedRequest(t *testing.T) {
	cfg := testConfig()
	_, addr := startRegistrar(t, cfg)
	client := newRegisterClient(t)
	recipient := recipientFor(t, addr)
	request := sip.NewRequest(sip.REGISTER, recipient)
	request.AppendHeader(&sip.FromHeader{Address: sip.Uri{Scheme: "sip", User: testDeviceID, Host: cfg.sipDomain}})
	request.AppendHeader(&sip.ToHeader{Address: sip.Uri{Scheme: "sip", User: testDeviceID, Host: cfg.sipDomain}})
	request.SetTransport("UDP")
	response := doSIP(t, client, request)
	if response.StatusCode != sip.StatusBadRequest {
		t.Fatalf("malformed status = %d", response.StatusCode)
	}
}

func TestRegisterRejectsNonSIPContact(t *testing.T) {
	cfg := testConfig()
	_, addr := startRegistrar(t, cfg)
	client := newRegisterClient(t)
	request := newRegisterRequest(t, cfg, addr, testDeviceID, 120, "192.0.2.10", 5060)
	request.Contact().Address.Scheme = "sips"
	if response := doSIP(t, client, request); response.StatusCode != sip.StatusBadRequest {
		t.Fatalf("status = %d", response.StatusCode)
	}
}

func TestRegisterResponseUsesReceivedAndRPort(t *testing.T) {
	cfg := testConfig()
	_, addr := startRegistrar(t, cfg)
	client := newRegisterClient(t)
	request := newRegisterRequest(t, cfg, addr, testDeviceID, 120, "192.0.2.10", 5060)
	response := doSIP(t, client, request)
	via := response.Via()
	if via == nil {
		t.Fatal("Via is missing")
	}
	if received, ok := via.Params.Get("received"); !ok || received != "127.0.0.1" {
		t.Fatalf("received = %q, %v", received, ok)
	}
	if rport, ok := via.Params.Get("rport"); !ok || rport == "" {
		t.Fatalf("rport = %q, %v", rport, ok)
	}
}

func testConfig() config {
	return config{
		sipListen:           "127.0.0.1:0",
		sipAdvertise:        "127.0.0.1:0",
		httpListen:          "127.0.0.1:0",
		sipID:               "34020000002000000001",
		sipDomain:           "3402000000",
		sipPassword:         "12345678",
		registerExpires:     time.Hour,
		heartbeatTimeout:    90 * time.Second,
		mediaServerTimeout:  15 * time.Second,
		mediaRequestTimeout: 3 * time.Second,
		inviteTimeout:       10 * time.Second,
		byeTimeout:          3 * time.Second,
	}
}

func startRegistrar(t *testing.T, cfg config) (*sipServer, string) {
	t.Helper()
	server, err := newSIPServer(cfg, slog.New(slog.NewTextHandler(io.Discard, nil)))
	if err != nil {
		t.Fatalf("newSIPServer() error = %v", err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	ready := make(chan string, 1)
	ctx = context.WithValue(ctx, sipgo.ListenReadyCtxKey, sipgo.ListenReadyFuncCtxValue(func(_ string, addr string) {
		ready <- addr
	}))
	done := make(chan error, 1)
	go func() { done <- server.serve(ctx) }()
	addr := waitReady(t, ready)
	t.Cleanup(func() {
		cancel()
		select {
		case err := <-done:
			if err != nil {
				t.Errorf("serve() error = %v", err)
			}
		case <-time.After(2 * time.Second):
			t.Error("SIP server did not stop")
		}
		server.close()
	})
	return server, addr
}

func newRegisterClient(t *testing.T) *sipgo.Client {
	t.Helper()
	ua, err := sipgo.NewUA(sipgo.WithUserAgent(testDeviceID), sipgo.WithUserAgentHostname("127.0.0.1"))
	if err != nil {
		t.Fatalf("NewUA() error = %v", err)
	}
	client, err := sipgo.NewClient(ua, sipgo.WithClientHostname("127.0.0.1"), sipgo.WithClientNAT())
	if err != nil {
		_ = ua.Close()
		t.Fatalf("NewClient() error = %v", err)
	}
	t.Cleanup(func() { _ = ua.Close() })
	return client
}

func newRegisterRequest(t *testing.T, cfg config, addr, deviceID string, expires uint32, contactHost string, contactPort int) *sip.Request {
	t.Helper()
	request := sip.NewRequest(sip.REGISTER, recipientFor(t, addr))
	fromParams := sip.NewParams()
	fromParams.Add("tag", fmt.Sprintf("tag-%d", time.Now().UnixNano()))
	request.AppendHeader(&sip.FromHeader{
		Address: sip.Uri{Scheme: "sip", User: deviceID, Host: cfg.sipDomain},
		Params:  fromParams,
	})
	request.AppendHeader(&sip.ToHeader{Address: sip.Uri{Scheme: "sip", User: deviceID, Host: cfg.sipDomain}})
	request.AppendHeader(&sip.ContactHeader{Address: sip.Uri{Scheme: "sip", User: deviceID, Host: contactHost, Port: contactPort}})
	expiresHeader := sip.ExpiresHeader(expires)
	request.AppendHeader(&expiresHeader)
	request.SetTransport("UDP")
	return request
}

func recipientFor(t *testing.T, addr string) sip.Uri {
	t.Helper()
	host, portText, err := net.SplitHostPort(addr)
	if err != nil {
		t.Fatalf("SplitHostPort() error = %v", err)
	}
	port, err := strconv.Atoi(portText)
	if err != nil {
		t.Fatalf("Atoi() error = %v", err)
	}
	return sip.Uri{Scheme: "sip", Host: host, Port: port}
}

func doSIP(t *testing.T, client *sipgo.Client, request *sip.Request) *sip.Response {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()
	response, err := client.Do(ctx, request)
	if err != nil {
		t.Fatalf("Client.Do() error = %v", err)
	}
	return response
}

func doDigest(t *testing.T, client *sipgo.Client, request *sip.Request, challenge *sip.Response, username, password string) *sip.Response {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()
	response, err := client.DoDigestAuth(ctx, request, challenge, sipgo.DigestAuth{Username: username, Password: password})
	if err != nil {
		t.Fatalf("DoDigestAuth() error = %v", err)
	}
	return response
}

func registerDevice(t *testing.T, client *sipgo.Client, cfg config, addr, deviceID string, expires uint32) {
	t.Helper()
	request := newRegisterRequest(t, cfg, addr, deviceID, expires, "192.0.2.10", 5060)
	challenge := doSIP(t, client, request)
	response := doDigest(t, client, request, challenge, deviceID, cfg.sipPassword)
	if response.StatusCode != sip.StatusOK {
		t.Fatalf("register status = %d", response.StatusCode)
	}
}
