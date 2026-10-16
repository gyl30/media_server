package main

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"log/slog"
	"net"
	"net/http"
	"net/http/httptest"
	"net/url"
	"path"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/emiago/sipgo"
	"github.com/emiago/sipgo/sip"
)

// 真实 UDP SIP；HTTP 只替代媒体控制，不将这些断言称作 PS 解码验收。
func TestPlaybackEndDuringAnswerUpdate(t *testing.T) {
	for _, test := range []struct {
		name         string
		method       sip.RequestMethod
		updateStatus int
	}{
		{"media_status", sip.MESSAGE, http.StatusNoContent},
		{"remote_bye", sip.BYE, http.StatusNoContent},
		{"media_status_then_update_failure", sip.MESSAGE, http.StatusBadGateway},
	} {
		t.Run(test.name, func(t *testing.T) {
			s := testInfrastructure(t)
			entered, release := make(chan struct{}), make(chan struct{})
			var unblock sync.Once
			var creates, deletes atomic.Int32
			media := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				switch r.URL.Path {
				case "/gb28181/receiver/playback/create":
					creates.Add(1)
					writeJSON(w, http.StatusCreated, map[string]int{"rtp_port": 30001})
				case "/gb28181/receiver/update":
					close(entered)
					<-release
					w.WriteHeader(test.updateStatus)
				case "/receivers/delete":
					deletes.Add(1)
					w.WriteHeader(http.StatusNoContent)
				case "/receivers":
					writeJSON(w, http.StatusOK, map[string]any{"receivers": []mediaReceiver{}})
				default:
					t.Errorf("unexpected media request %s", r.URL.Path)
					w.WriteHeader(http.StatusNotFound)
				}
			}))
			defer media.Close()
			s.media.server.controlURL = media.URL
			s.media.client.Timeout = 5 * time.Second
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
				_ = platform.Close()
				select {
				case <-serverDone:
				case <-time.After(2 * time.Second):
					t.Error("SIP server did not exit")
				}
			}()
			client, err := sipgo.NewClient(s.live.sip.ua, sipgo.WithClientAddr("127.0.0.1:0"), sipgo.WithClientConnectionAddr("127.0.0.1:0"))
			if err != nil {
				t.Fatal(err)
			}
			s.live.sip.client = client
			const deviceID = "34020000001320000001"
			const channelID = "34020000001320000002"
			host, portText, _ := net.SplitHostPort(peer.LocalAddr().String())
			port, _ := strconv.Atoi(portText)
			contact := sip.Uri{Scheme: "sip", User: deviceID, Host: host, Port: port}
			if err := s.live.sip.deviceStore.create(t.Context(), gbDevice{deviceID: deviceID, name: "playback end test"}); err != nil {
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
			defer func() {
				unblock.Do(func() { close(release) })
				ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
				defer cancel()
				s.live.shutdown(ctx)
			}()
			ticket, err := s.startPlayback(t.Context(), deviceID, channelID, 1700000000, 1700000002, sessionPlayback, 1)
			if err != nil {
				t.Fatal(err)
			}
			u, err := url.Parse(ticket.PSURL)
			if err != nil {
				t.Fatal(err)
			}
			s.live.mu.Lock()
			session := s.live.sessions[liveKey{deviceID: deviceID, channelID: channelID, historyID: ticket.PlaybackID}]
			s.live.mu.Unlock()
			if session == nil {
				t.Fatal("prepared session missing")
			}
			verifyToken(t, s, strings.TrimSuffix(path.Base(u.Path), ".ps"), "play", ticket.PlaybackID, http.StatusOK)
			buffer := make([]byte, 65536)
			read := func() (sip.Message, net.Addr) {
				t.Helper()
				_ = peer.SetReadDeadline(time.Now().Add(3 * time.Second))
				n, address, err := peer.ReadFrom(buffer)
				if err != nil {
					t.Fatal(err)
				}
				message, err := sip.ParseMessage(buffer[:n])
				if err != nil {
					t.Fatal(err)
				}
				return message, address
			}
			message, destination := read()
			invite, ok := message.(*sip.Request)
			if !ok || invite.Method != sip.INVITE {
				t.Fatalf("expected INVITE, got %v", message)
			}
			answerBody := bytes.ReplaceAll(invite.Body(), []byte("a=recvonly"), []byte("a=sendonly"))
			answerBody = bytes.ReplaceAll(answerBody, []byte("a=setup:passive"), []byte("a=setup:active"))
			answerBody = bytes.ReplaceAll(answerBody, []byte(fmt.Sprintf("y=%010d", session.ssrc)), []byte("y=1000000999"))
			answer := sip.NewResponseFromRequest(invite, sip.StatusOK, "OK", answerBody)
			answer.To().Params.Add("tag", "playback-end-peer")
			answer.AppendHeader(&sip.ContactHeader{Address: contact})
			answer.AppendHeader(sip.NewHeader("Content-Type", "application/sdp"))
			if _, err := peer.WriteTo([]byte(answer.String()), destination); err != nil {
				t.Fatal(err)
			}
			message, _ = read()
			ack, ok := message.(*sip.Request)
			if !ok || ack.Method != sip.ACK {
				t.Fatalf("expected ACK, got %v", message)
			}
			select {
			case <-entered:
			case <-time.After(2 * time.Second):
				t.Fatal("SSRC update barrier not reached")
			}
			select {
			case <-session.established:
				t.Fatal("setup finished before blocked SSRC update")
			default:
			}
			sequence := uint32(0)
			endRequest := func(method sip.RequestMethod, channel, tag, call string, notify int, want int) {
				t.Helper()
				sequence++
				request := sip.NewRequest(method, sip.Uri{Scheme: "sip", User: s.live.sip.cfg.sipID, Host: "127.0.0.1"})
				request.AppendHeader(sip.NewHeader("Via", fmt.Sprintf("SIP/2.0/UDP %s;branch=z9hG4bKend-%d", peer.LocalAddr(), sequence)))
				from := &sip.FromHeader{Address: answer.To().Address, Params: answer.To().Params.Clone()}
				if tag != "" {
					from.Params.Add("tag", tag)
				}
				request.AppendHeader(from)
				request.AppendHeader(&sip.ToHeader{Address: invite.From().Address, Params: invite.From().Params.Clone()})
				id := sip.CallIDHeader(call)
				request.AppendHeader(&id)
				request.AppendHeader(&sip.CSeqHeader{SeqNo: sequence, MethodName: method})
				maxForwards := sip.MaxForwardsHeader(70)
				request.AppendHeader(&maxForwards)
				if method == sip.MESSAGE {
					request.AppendHeader(sip.NewHeader("Content-Type", "Application/MANSCDP+xml"))
					request.SetBody([]byte(fmt.Sprintf("<Notify><CmdType>MediaStatus</CmdType><SN>%d</SN><DeviceID>%s</DeviceID><NotifyType>%d</NotifyType></Notify>", sequence, channel, notify)))
				} else {
					request.SetBody(nil)
				}
				if _, err := peer.WriteTo([]byte(request.String()), platform.LocalAddr()); err != nil {
					t.Fatal(err)
				}
				message, _ := read()
				response, ok := message.(*sip.Response)
				if !ok || response.StatusCode != want || response.CSeq().SeqNo != sequence || response.CSeq().MethodName != method {
					t.Fatalf("end response got %v want %d, sequence %d", message, want, sequence)
				}
			}
			call := invite.CallID().Value()
			endRequest(test.method, channelID, "wrong-tag", call, 121, 481)
			endRequest(test.method, channelID, "", "unrelated-call", 121, 481)
			if test.method == sip.MESSAGE {
				endRequest(test.method, deviceID, "", call, 121, 481)
				endRequest(test.method, channelID, "", call, 122, 400)
			}
			endRequest(test.method, channelID, "", call, 121, 200)
			if deletes.Load() != 0 {
				t.Fatal("end notification deleted receiver before setup completed")
			}
			select {
			case <-session.established:
				t.Fatal("end handler bypassed setup barrier")
			default:
			}
			unblock.Do(func() { close(release) })
			byes := 0
			if test.method == sip.MESSAGE || test.updateStatus != http.StatusNoContent {
				message, address := read()
				bye, ok := message.(*sip.Request)
				if !ok || bye.Method != sip.BYE {
					t.Fatalf("expected BYE got %v", message)
				}
				byes++
				if _, err := peer.WriteTo([]byte(sip.NewResponseFromRequest(bye, 200, "OK", nil).String()), address); err != nil {
					t.Fatal(err)
				}
			}
			select {
			case <-session.established:
			case <-time.After(2 * time.Second):
				t.Fatal("setup did not finish")
			}
			if test.updateStatus == http.StatusNoContent {
				deadline := time.NewTimer(2 * time.Second)
				ticker := time.NewTicker(5 * time.Millisecond)
				defer deadline.Stop()
				defer ticker.Stop()
				for {
					s.live.mu.Lock()
					stopping := s.live.sessions[session.key] == session && session.state == liveStopping
					s.live.mu.Unlock()
					if stopping {
						break
					}
					select {
					case <-ticker.C:
					case <-deadline.C:
						t.Fatal("end notification never entered drain")
					}
				}
				if deletes.Load() != 0 {
					t.Fatal("receiver deleted without transport EOF or drain deadline")
				}
				if err := s.reconcileMedia(t.Context()); err != nil {
					t.Fatal(err)
				}
			}
			select {
			case <-session.done:
			case <-time.After(2 * time.Second):
				t.Fatal("end cleanup did not finish")
			}
			s.live.mu.Lock()
			remaining := len(s.live.sessions)
			s.live.mu.Unlock()
			s.live.ssrcs.mu.Lock()
			ssrcs := len(s.live.ssrcs.active)
			s.live.ssrcs.mu.Unlock()
			if remaining != 0 || ssrcs != 0 || creates.Load() != 1 || deletes.Load() != 1 {
				t.Fatalf("cleanup sessions=%d SSRCs=%d create=%d delete=%d", remaining, ssrcs, creates.Load(), deletes.Load())
			}
			t.Logf("ACK-before-update end=%s:200 setup-status=%d BYE=%d receiver create/delete=1/1 sessions=0 SSRCs=0", test.method, test.updateStatus, byes)
		})
	}
}

func TestPlaybackDrainTerminatesOnce(t *testing.T) {
	for _, action := range []string{"delete", "device_offline", "shutdown", "reconcile", "deadline"} {
		t.Run(action, func(t *testing.T) {
			s := testInfrastructure(t)
			var logs bytes.Buffer
			s.live.logger = slog.New(slog.NewTextHandler(&logs, nil))
			var deletes atomic.Int32
			media := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				switch r.URL.Path {
				case "/receivers/delete":
					deletes.Add(1)
					w.WriteHeader(http.StatusNoContent)
				case "/receivers":
					writeJSON(w, http.StatusOK, map[string]any{"receivers": []mediaReceiver{}})
				default:
					t.Errorf("unexpected media request %s", r.URL.Path)
					w.WriteHeader(http.StatusNotFound)
				}
			}))
			defer media.Close()
			s.media.server.controlURL = media.URL
			ssrc, err := s.live.ssrcs.acquire(playbackReceiver)
			if err != nil {
				t.Fatal(err)
			}
			ctx, cancel := context.WithCancel(t.Context())
			defer cancel()
			const id = "11111111-1111-4111-8111-111111111111"
			established := make(chan struct{})
			close(established)
			session := &liveSession{
				key:      liveKey{deviceID: gbPlaybackDevice, channelID: gbPlaybackChannel, historyID: id},
				streamID: id, ssrc: ssrc, state: liveStreaming, ctx: ctx, cancel: cancel,
				established: established, done: make(chan struct{}),
			}
			s.live.mu.Lock()
			s.live.sessions[session.key] = session
			s.live.mu.Unlock()
			defer func() {
				cleanupContext, cleanupCancel := context.WithTimeout(context.Background(), 3*time.Second)
				defer cleanupCancel()
				s.live.shutdown(cleanupContext)
			}()
			// 白盒终态交错；SIP dialog/应答由上面的真实 UDP 三组覆盖。
			var endings sync.WaitGroup
			for range 2 {
				endings.Go(func() { s.live.finishPlayback(session, false) })
			}
			endings.Wait()
			if deletes.Load() != 0 {
				t.Fatal("drain deleted receiver before termination trigger")
			}
			started := time.Now()
			switch action {
			case "delete":
				apiRequest(t, s.handler(), "DELETE", "/api/playbacks/"+id, "", http.StatusNoContent)
			case "device_offline":
				s.live.mu.Lock()
				sessions := s.live.beginDeviceStopLocked(gbPlaybackDevice)
				s.live.mu.Unlock()
				s.live.finishDeviceOffline(gbPlaybackDevice, sessions)
			case "shutdown":
				s.live.shutdown(t.Context())
			case "reconcile":
				if err := s.reconcileMedia(t.Context()); err != nil {
					t.Fatal(err)
				}
			case "deadline":
				// 真实生产 20 秒 timer，不以 shutdown 或缩短配置冒充耗尽。
			}
			limit := 3 * time.Second
			if action == "deadline" {
				limit = playbackDrainTimeout + 3*time.Second
			}
			select {
			case <-session.done:
			case <-time.After(limit):
				t.Fatal("drain did not terminate")
			}
			gbPlaybackResources(t, s, 0, 0, 0)
			if deletes.Load() != 1 {
				t.Fatalf("duplicate end caused %d deletes", deletes.Load())
			}
			if action == "deadline" && (!strings.Contains(logs.String(), "transfer may be incomplete") || time.Since(started) < playbackDrainTimeout-time.Second) {
				t.Fatalf("drain did not report actual bounded timeout: elapsed=%v logs=%s", time.Since(started), logs.String())
			}
			t.Logf("duplicate end, trigger=%s, elapsed=%v, receiver delete=1, sessions/tokens/SSRC=0/0/0", action, time.Since(started))
		})
	}
}

type playbackFailedResponse struct {
	header http.Header
	status int
}

func (w *playbackFailedResponse) Header() http.Header    { return w.header }
func (w *playbackFailedResponse) WriteHeader(status int) { w.status = status }
func (w *playbackFailedResponse) Write([]byte) (int, error) {
	return 0, errors.New("injected response write failure")
}

func TestPlaybackCreatedResponseWriteFailure(t *testing.T) {
	s := testInfrastructure(t)
	var deletes atomic.Int32
	media := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/gb28181/receiver/playback/create":
			writeJSON(w, http.StatusCreated, map[string]int{"rtp_port": 30001})
		case "/receivers/delete":
			deletes.Add(1)
			w.WriteHeader(http.StatusNoContent)
		default:
			t.Errorf("unexpected request %s", r.URL.Path)
			w.WriteHeader(http.StatusNotFound)
		}
	}))
	defer media.Close()
	s.media.server.controlURL = media.URL
	invites, _ := gbPlaybackPeer(t, s)
	request := httptest.NewRequest("POST", gbPlaybackPath, strings.NewReader(gbPlaybackBody))
	request.Header.Set("Content-Type", "application/json")
	response := &playbackFailedResponse{header: make(http.Header)}
	s.handler().ServeHTTP(response, request)
	if response.status != http.StatusCreated || deletes.Load() != 1 || invites.Load() != 0 {
		t.Fatalf("write failure status=%d delete=%d INVITE=%d", response.status, deletes.Load(), invites.Load())
	}
	gbPlaybackResources(t, s, 0, 0, 0)
}
