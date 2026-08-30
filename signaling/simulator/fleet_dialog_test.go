package main

import (
	"errors"
	"io"
	"log/slog"
	"sync"
	"testing"

	"github.com/emiago/sipgo/sip"
)

type cancelOnTryingTransaction struct {
	done        chan struct{}
	acks        chan *sip.Request
	canceled    chan struct{}
	release     chan struct{}
	cancel      []sip.FnTxCancel
	termination []sip.FnTxTerminate
	err         error
	closeOnce   sync.Once
}

func newCancelOnTryingTransaction() *cancelOnTryingTransaction {
	return &cancelOnTryingTransaction{
		done: make(chan struct{}), acks: make(chan *sip.Request), canceled: make(chan struct{}), release: make(chan struct{}),
	}
}

func (t *cancelOnTryingTransaction) Terminate() {
	t.closeOnce.Do(func() { close(t.done) })
}

func (t *cancelOnTryingTransaction) OnTerminate(callback sip.FnTxTerminate) bool {
	t.termination = append(t.termination, callback)
	return true
}

func (t *cancelOnTryingTransaction) Done() <-chan struct{} {
	return t.done
}

func (t *cancelOnTryingTransaction) Err() error {
	return t.err
}

func (t *cancelOnTryingTransaction) Respond(response *sip.Response) error {
	if response.StatusCode != sip.StatusTrying {
		return nil
	}
	t.err = sip.ErrTransactionCanceled
	cancel := sip.NewRequest(sip.CANCEL, sip.Uri{})
	for _, callback := range t.cancel {
		callback(cancel)
	}
	close(t.canceled)
	<-t.release
	t.Terminate()
	return t.err
}

func (t *cancelOnTryingTransaction) Acks() <-chan *sip.Request {
	return t.acks
}

func (t *cancelOnTryingTransaction) OnCancel(callback sip.FnTxCancel) bool {
	t.cancel = append(t.cancel, callback)
	return true
}

func TestFleetCancelRemovesPendingDialogBeforeInviteHandlerReturns(t *testing.T) {
	identities, err := newIdentitySet("34020000001320000001", "34020000001320000002", 1)
	if err != nil {
		t.Fatal(err)
	}
	fleet := &simulatedFleet{
		logger:     slog.New(slog.NewTextHandler(io.Discard, nil)),
		identities: identities,
		endpoints:  []fleetEndpoint{{listenAddr: "127.0.0.1:5062"}},
		states:     make([]fleetDeviceState, 1),
		dialogs:    make(map[string]int),
		errors:     make(chan error, 1),
	}
	request := sip.NewRequest(sip.INVITE, sip.Uri{Scheme: "sip", User: identities.channelID(0), Host: "127.0.0.1"})
	viaParams := sip.NewParams()
	viaParams.Add("branch", "z9hG4bK-cancel-test")
	request.AppendHeader(&sip.ViaHeader{ProtocolName: "SIP", ProtocolVersion: "2.0", Transport: "UDP", Host: "127.0.0.1", Port: 5060, Params: viaParams})
	fromParams := sip.NewParams()
	fromParams.Add("tag", "from-tag")
	request.AppendHeader(&sip.FromHeader{Address: sip.Uri{Scheme: "sip", User: "34020000002000000001", Host: "127.0.0.1"}, Params: fromParams})
	request.AppendHeader(&sip.ToHeader{Address: sip.Uri{Scheme: "sip", User: identities.channelID(0), Host: "127.0.0.1"}, Params: sip.NewParams()})
	callID := sip.CallIDHeader("cancel-test")
	request.AppendHeader(&callID)
	request.AppendHeader(&sip.CSeqHeader{SeqNo: 1, MethodName: sip.INVITE})
	request.AppendHeader(&sip.ContactHeader{Address: sip.Uri{Scheme: "sip", User: "34020000002000000001", Host: "127.0.0.1", Port: 5060}})
	request.SetTransport("UDP")
	request.SetBody([]byte("v=0\r\no=34020000001320000002 0 0 IN IP4 127.0.0.1\r\ns=Play\r\nc=IN IP4 127.0.0.1\r\nt=0 0\r\nm=video 40000 RTP/AVP 96\r\na=recvonly\r\na=rtpmap:96 PS/90000\r\ny=0200000001\r\n"))

	transaction := newCancelOnTryingTransaction()
	t.Cleanup(func() {
		select {
		case <-transaction.release:
		default:
			close(transaction.release)
		}
	})
	handlerDone := make(chan struct{})
	go func() {
		fleet.handleInvite(0, request, transaction)
		close(handlerDone)
	}()
	<-transaction.canceled
	fleet.mutex.Lock()
	dialog := fleet.states[0].dialog
	fleet.mutex.Unlock()
	close(transaction.release)
	<-handlerDone
	if dialog != nil {
		t.Fatal("CANCEL left the pending dialog occupied")
	}
	if err := <-fleet.errors; !errors.Is(err, sip.ErrTransactionCanceled) {
		t.Fatalf("handler error = %v", err)
	}
}
