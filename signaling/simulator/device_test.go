package main

import (
	"context"
	"io"
	"log/slog"
	"net"
	"testing"

	"github.com/emiago/sipgo"
	"github.com/emiago/sipgo/sip"
)

func TestDeviceClientReusesSIPListener(t *testing.T) {
	platformUA, err := sipgo.NewUA()
	if err != nil {
		t.Fatal(err)
	}
	defer platformUA.Close()
	platform, err := sipgo.NewServer(platformUA)
	if err != nil {
		t.Fatal(err)
	}
	source := make(chan string, 1)
	platform.OnMessage(func(request *sip.Request, transaction sip.ServerTransaction) {
		source <- request.Source()
		_ = transaction.Respond(sip.NewResponseFromRequest(request, sip.StatusOK, "OK", nil))
	})
	ready := make(chan string, 1)
	serveContext, cancel := context.WithCancel(t.Context())
	serveContext = context.WithValue(serveContext, sipgo.ListenReadyCtxKey, sipgo.ListenReadyFuncCtxValue(func(_ string, address string) {
		ready <- address
	}))
	done := make(chan error, 1)
	go func() { done <- platform.ListenAndServe(serveContext, "udp", "127.0.0.1:0") }()
	platformAddress := <-ready

	cfg, err := parseConfig([]string{"--listen", "127.0.0.1:0", "--platform-sip", platformAddress})
	if err != nil {
		t.Fatal(err)
	}
	device, err := newSimulatedDevice(cfg, slog.New(slog.NewTextHandler(io.Discard, nil)))
	if err != nil {
		t.Fatal(err)
	}
	if err := device.start(t.Context()); err != nil {
		t.Fatal(err)
	}
	defer device.close()
	if err := device.keepalive(t.Context(), 1); err != nil {
		t.Fatal(err)
	}
	actual := <-source
	_, actualPort, err := net.SplitHostPort(actual)
	if err != nil {
		t.Fatal(err)
	}
	_, listenPort, err := net.SplitHostPort(device.listenAddr)
	if err != nil {
		t.Fatal(err)
	}
	if actualPort != listenPort {
		t.Fatalf("MESSAGE source = %s, SIP listener = %s", actual, device.listenAddr)
	}
	cancel()
	<-done
}

func TestAckOutsideDialogIsNotAccepted(t *testing.T) {
	device := &simulatedDevice{
		logger: slog.New(slog.NewTextHandler(io.Discard, nil)),
		ack:    make(chan struct{}, 1),
	}
	device.dialogs.Store(sipgo.NewDialogServerCache(nil, sip.ContactHeader{}))
	request := sip.NewRequest(sip.ACK, sip.Uri{Scheme: "sip", User: "34020000001320000002", Host: "127.0.0.1"})
	callID := sip.CallIDHeader("outside-dialog")
	fromParams := sip.NewParams()
	fromParams.Add("tag", "from-tag")
	toParams := sip.NewParams()
	toParams.Add("tag", "to-tag")
	request.AppendHeader(&callID)
	request.AppendHeader(&sip.FromHeader{Address: sip.Uri{Scheme: "sip", User: "34020000001320000001", Host: "127.0.0.1"}, Params: fromParams})
	request.AppendHeader(&sip.ToHeader{Address: sip.Uri{Scheme: "sip", User: "34020000001320000002", Host: "127.0.0.1"}, Params: toParams})
	request.AppendHeader(&sip.CSeqHeader{SeqNo: 1, MethodName: sip.ACK})

	device.handleAck(request, nil)
	select {
	case <-device.ack:
		t.Fatal("ACK outside a dialog was accepted")
	default:
	}
}
