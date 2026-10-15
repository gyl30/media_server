package main

import (
	"bytes"
	"context"
	"encoding/json"
	"net"
	"net/http"
	"net/http/httptest"
	"strconv"
	"sync/atomic"
	"testing"
	"time"

	"github.com/emiago/sipgo"
	"github.com/emiago/sipgo/sip"
)

// SIP 请求和响应均经过真实 UDP；媒体 HTTP 夹具只验证 receiver 的控制与回收。
func TestLiveRemoteByeKeepsInviteRetransmissionACK(t *testing.T) {
	s := testInfrastructure(t)
	var creates, deletes atomic.Int32
	var receiverID atomic.Value
	media := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		var body struct {
			StreamID string `json:"stream_id"`
		}
		if r.Method != http.MethodPost || json.NewDecoder(r.Body).Decode(&body) != nil || body.StreamID == "" {
			t.Errorf("invalid media request: %s %s", r.Method, r.URL.Path)
			w.WriteHeader(http.StatusBadRequest)
			return
		}
		switch r.URL.Path {
		case "/gb28181/receiver/create":
			receiverID.Store(body.StreamID)
			creates.Add(1)
			writeJSON(w, http.StatusCreated, map[string]int{"rtp_port": 30000})
		case "/receivers/delete":
			if receiverID.Load() != body.StreamID {
				t.Error("deleted receiver differs from created receiver")
				w.WriteHeader(http.StatusBadRequest)
				return
			}
			deletes.Add(1)
			w.WriteHeader(http.StatusNoContent)
		default:
			t.Errorf("unexpected media request: %s", r.URL.Path)
			w.WriteHeader(http.StatusNotFound)
		}
	}))
	defer media.Close()
	s.media.server.controlURL = media.URL
	peer, err := net.ListenPacket("udp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer peer.Close()
	platform, err := net.ListenPacket("udp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	serverDone := make(chan error, 1)
	go func() { serverDone <- s.live.sip.server.ServeUDP(platform) }()
	defer func() {
		_ = s.live.sip.ua.Close()
		_ = platform.Close()
		select {
		case <-serverDone:
		case <-time.After(2 * time.Second):
			t.Error("platform UDP server did not exit")
		}
	}()
	// 独立动态出站端口，避免测试依赖固定配置端口或 ServeUDP 注册连接池的时序。
	client, err := sipgo.NewClient(s.live.sip.ua,
		sipgo.WithClientAddr("127.0.0.1:0"), sipgo.WithClientConnectionAddr("127.0.0.1:0"))
	if err != nil {
		t.Fatal(err)
	}
	s.live.sip.client = client
	const deviceID = "34020000001320000001"
	const channelID = "34020000001320000002"
	host, portText, _ := net.SplitHostPort(peer.LocalAddr().String())
	port, _ := strconv.Atoi(portText)
	contact := sip.Uri{Scheme: "sip", User: deviceID, Host: host, Port: port}
	if err := s.live.sip.deviceStore.create(t.Context(), gbDevice{deviceID: deviceID, name: "remote BYE test"}); err != nil {
		t.Fatal(err)
	}
	s.live.sip.devices.register(registeredDevice{id: deviceID, contact: contact, remoteEndpoint: peer.LocalAddr().String(), online: true, expiresAt: time.Now().Add(time.Hour), lastHeartbeat: time.Now()})
	s.live.sip.channels.beginQuery(deviceID, 1)
	catalog := catalogResponse{DeviceID: deviceID, SN: 1, SumNum: 1}
	catalog.DeviceList.Num = 1
	catalog.DeviceList.Items = []catalogChannel{{DeviceID: channelID, Status: "ON"}}
	if err := s.live.sip.channels.apply(catalog); err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithCancel(t.Context())
	type result struct {
		view liveView
		err  error
	}
	completed := make(chan result, 1)
	startDone := make(chan struct{})
	go func() {
		defer close(startDone)
		view, err := s.live.startLive(ctx, deviceID, channelID)
		completed <- result{view, err}
	}()
	defer func() {
		cancel()
		// 失败路径也须先终止在途事务并 join owner，再关闭媒体 HTTP 夹具。
		_ = s.live.sip.ua.Close()
		select {
		case <-startDone:
		case <-time.After(2 * time.Second):
			t.Error("start owner did not exit")
		}
		cleanupContext, cleanupCancel := context.WithTimeout(context.Background(), 2*time.Second)
		defer cleanupCancel()
		s.live.shutdown(cleanupContext)
	}()
	buffer := make([]byte, 65536)
	read := func(deadline time.Time) (sip.Message, net.Addr, error) {
		t.Helper()
		if err := peer.SetReadDeadline(deadline); err != nil {
			return nil, nil, err
		}
		n, address, err := peer.ReadFrom(buffer)
		if err != nil {
			return nil, nil, err
		}
		message, err := sip.ParseMessage(buffer[:n])
		return message, address, err
	}
	message, destination, err := read(time.Now().Add(2 * time.Second))
	if err != nil {
		t.Fatal(err)
	}
	invite, ok := message.(*sip.Request)
	if !ok || invite.Method != sip.INVITE || invite.CSeq() == nil || invite.Contact() == nil {
		t.Fatalf("expected complete INVITE, got %v", message)
	}
	answer := sip.NewResponseFromRequest(invite, sip.StatusOK, "OK", bytes.ReplaceAll(invite.Body(), []byte("a=recvonly"), []byte("a=sendonly")))
	answer.To().Params.Add("tag", "remote-bye-peer")
	answer.AppendHeader(&sip.ContactHeader{Address: contact})
	answer.AppendHeader(sip.NewHeader("Content-Type", "application/sdp"))
	sendAnswer := func() {
		t.Helper()
		if _, err := peer.WriteTo([]byte(answer.String()), destination); err != nil {
			t.Fatal(err)
		}
	}
	acks, byes := 0, 0
	readACK := func() {
		t.Helper()
		deadline := time.Now().Add(2 * time.Second)
		for {
			message, address, err := read(deadline)
			if err != nil {
				t.Fatalf("ACK not received: %v", err)
			}
			request, ok := message.(*sip.Request)
			if !ok {
				t.Fatalf("expected ACK, got %v", message)
			}
			if request.Method == sip.BYE {
				byes++
				t.Error("remote BYE incorrectly triggered a reverse BYE")
				_, _ = peer.WriteTo([]byte(sip.NewResponseFromRequest(request, 200, "OK", nil).String()), address)
				continue
			}
			if request.Method != sip.ACK || request.CSeq() == nil || request.CSeq().SeqNo != invite.CSeq().SeqNo || request.CSeq().MethodName != sip.ACK {
				t.Fatalf("ACK did not preserve INVITE CSeq: %v", request)
			}
			expectedID, expectedErr := sip.DialogIDFromResponse(answer)
			actualID, actualErr := sip.DialogIDFromRequestUAS(request)
			if expectedErr != nil || actualErr != nil || actualID != expectedID {
				t.Fatalf("ACK belongs to another dialog: %q/%v != %q/%v", actualID, actualErr, expectedID, expectedErr)
			}
			acks++
			return
		}
	}
	sendAnswer()
	readACK()
	var started result
	select {
	case started = <-completed:
	case <-time.After(2 * time.Second):
		t.Fatal("start did not finish after ACK")
	}
	if started.err != nil || started.view.state != liveStreaming {
		t.Fatalf("start failed: %+v %v", started.view, started.err)
	}
	s.live.mu.Lock()
	session := s.live.sessions[liveKey{deviceID: deviceID, channelID: channelID}]
	s.live.mu.Unlock()
	if session == nil || session.streamID != started.view.streamID {
		t.Fatal("streaming session missing before remote BYE")
	}
	s.live.ssrcs.mu.Lock()
	_, allocated := s.live.ssrcs.active[session.ssrc%10000]
	s.live.ssrcs.mu.Unlock()
	if !allocated {
		t.Fatal("session SSRC was not allocated")
	}
	remoteBye := sip.NewRequest(sip.BYE, invite.Contact().Address)
	remoteBye.AppendHeader(sip.NewHeader("Via", "SIP/2.0/UDP "+peer.LocalAddr().String()+";branch=z9hG4bKremote-bye"))
	remoteBye.AppendHeader(&sip.FromHeader{Address: answer.To().Address, Params: answer.To().Params.Clone()})
	remoteBye.AppendHeader(&sip.ToHeader{Address: invite.From().Address, Params: invite.From().Params.Clone()})
	remoteBye.AppendHeader(sip.HeaderClone(invite.CallID()))
	remoteBye.AppendHeader(&sip.CSeqHeader{SeqNo: 1, MethodName: sip.BYE})
	maxForwards := sip.MaxForwardsHeader(70)
	remoteBye.AppendHeader(&maxForwards)
	remoteBye.SetBody(nil)
	if _, err := peer.WriteTo([]byte(remoteBye.String()), platform.LocalAddr()); err != nil {
		t.Fatal(err)
	}
	message, _, err = read(time.Now().Add(2 * time.Second))
	if err != nil {
		t.Fatal(err)
	}
	byeResponse, ok := message.(*sip.Response)
	if !ok || byeResponse.StatusCode != sip.StatusOK || byeResponse.CallID() == nil || byeResponse.CallID().Value() != invite.CallID().Value() || byeResponse.CSeq() == nil || byeResponse.CSeq().MethodName != sip.BYE || byeResponse.CSeq().SeqNo != 1 {
		t.Fatalf("remote BYE did not receive matching 200: %v", message)
	}
	select {
	case <-session.done:
	case <-time.After(2 * time.Second):
		t.Fatal("remote BYE did not finish local cleanup")
	}
	s.live.mu.Lock()
	remaining := len(s.live.sessions)
	s.live.mu.Unlock()
	s.live.ssrcs.mu.Lock()
	remainingSSRCs := len(s.live.ssrcs.active)
	s.live.ssrcs.mu.Unlock()
	if remaining != 0 || remainingSSRCs != 0 || creates.Load() != 1 || deletes.Load() != 1 {
		t.Fatalf("remote BYE cleanup: sessions=%d SSRCs=%d receiver create=%d delete=%d", remaining, remainingSSRCs, creates.Load(), deletes.Load())
	}
	// 资源已释放后，事务尾期仍须独立应答原 INVITE 成功响应的重传。
	sendAnswer()
	readACK()
	message, _, err = read(time.Now().Add(100 * time.Millisecond))
	if err == nil {
		if request, ok := message.(*sip.Request); ok && request.Method == sip.BYE {
			byes++
		}
		t.Errorf("unexpected packet after retransmission ACK: %v", message)
	} else if timeout, ok := err.(net.Error); !ok || !timeout.Timeout() {
		t.Errorf("UDP observation failed: %v", err)
	}
	if acks != 2 || byes != 0 {
		t.Errorf("ACK=%d reverse BYE=%d", acks, byes)
	}
	t.Logf("remote BYE=200 ACK=%d original CSeq=%d reverse BYE=%d sessions=%d SSRCs=%d receiver create=%d delete=%d", acks, invite.CSeq().SeqNo, byes, remaining, remainingSSRCs, creates.Load(), deletes.Load())
}
