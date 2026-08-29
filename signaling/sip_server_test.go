package main

import (
	"context"
	"io"
	"log/slog"
	"net"
	"strings"
	"testing"
	"time"

	"github.com/emiago/sipgo"
)

func TestSIPServerServesUDP(t *testing.T) {
	server := newTestSIPServer(t, "127.0.0.1:0")
	ctx, cancel := context.WithCancel(context.Background())
	ready := make(chan string, 1)
	ctx = context.WithValue(ctx, sipgo.ListenReadyCtxKey, sipgo.ListenReadyFuncCtxValue(func(_ string, addr string) {
		ready <- addr
	}))
	done := make(chan error, 1)
	go func() { done <- server.serve(ctx) }()

	addr := waitReady(t, ready)
	conn, err := net.Dial("udp", addr)
	if err != nil {
		t.Fatalf("net.Dial() error = %v", err)
	}
	defer conn.Close()
	if err := conn.SetDeadline(time.Now().Add(2 * time.Second)); err != nil {
		t.Fatalf("SetDeadline() error = %v", err)
	}
	request := "OPTIONS sip:34020000002000000001@127.0.0.1 SIP/2.0\r\n" +
		"Via: SIP/2.0/UDP 127.0.0.1:5061;branch=z9hG4bK-stage0;rport\r\n" +
		"From: <sip:34020000001320000001@127.0.0.1>;tag=stage0\r\n" +
		"To: <sip:34020000002000000001@127.0.0.1>\r\n" +
		"Call-ID: stage0@127.0.0.1\r\n" +
		"CSeq: 1 OPTIONS\r\n" +
		"Max-Forwards: 70\r\n" +
		"Content-Length: 0\r\n\r\n"
	if _, err := conn.Write([]byte(request)); err != nil {
		t.Fatalf("Write() error = %v", err)
	}
	response := make([]byte, 2048)
	n, err := conn.Read(response)
	if err != nil {
		t.Fatalf("Read() error = %v", err)
	}
	if !strings.HasPrefix(string(response[:n]), "SIP/2.0 405") {
		t.Fatalf("response = %q", response[:n])
	}

	cancel()
	if err := <-done; err != nil {
		t.Fatalf("serve() error = %v", err)
	}
}

func TestSIPServerReportsUDPBindFailure(t *testing.T) {
	conn, err := net.ListenPacket("udp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("ListenPacket() error = %v", err)
	}
	defer conn.Close()

	server := newTestSIPServer(t, conn.LocalAddr().String())
	if err := server.serve(context.Background()); err == nil {
		t.Fatal("serve() succeeded")
	}
}

func TestSIPServerStopsWithContext(t *testing.T) {
	server := newTestSIPServer(t, "127.0.0.1:0")
	ctx, cancel := context.WithCancel(context.Background())
	ready := make(chan string, 1)
	ctx = context.WithValue(ctx, sipgo.ListenReadyCtxKey, sipgo.ListenReadyFuncCtxValue(func(_ string, addr string) {
		ready <- addr
	}))
	done := make(chan error, 1)
	go func() { done <- server.serve(ctx) }()

	waitReady(t, ready)
	cancel()
	select {
	case err := <-done:
		if err != nil {
			t.Fatalf("serve() error = %v", err)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("serve() did not stop")
	}
}

func newTestSIPServer(t *testing.T, listen string) *sipServer {
	t.Helper()
	cfg := testConfig()
	cfg.sipListen = listen
	server, err := newSIPServer(cfg, slog.New(slog.NewTextHandler(io.Discard, nil)))
	if err != nil {
		t.Fatalf("newSIPServer() error = %v", err)
	}
	t.Cleanup(func() { server.close() })
	return server
}

func waitReady(t *testing.T, ready <-chan string) string {
	t.Helper()
	select {
	case addr := <-ready:
		return addr
	case <-time.After(2 * time.Second):
		t.Fatal("SIP server did not start")
		return ""
	}
}
