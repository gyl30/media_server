package main

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
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

func TestLiveInviteProtocolBoundaries(t *testing.T) {
	for _, scenario := range []struct {
		name       string
		early      bool
		successful bool
	}{
		{name: "cancel_before_first_provisional", early: true},
		{name: "cancel_after_100_and_487_retransmission"},
		{name: "twelve_provisionals_then_success", successful: true},
	} {
		t.Run(scenario.name, func(t *testing.T) {
			s := testInfrastructure(t)
			client, err := sipgo.NewClient(s.live.sip.ua,
				sipgo.WithClientAddr("127.0.0.1:0"), sipgo.WithClientConnectionAddr("127.0.0.1:0"))
			if err != nil {
				t.Fatal(err)
			}
			s.live.sip.client = client
			s.live.inviteTimeout = 2 * time.Second
			s.live.byeTimeout = time.Second
			var creates, deletes, cancelPackets, cancelTransactions, acks, byes, provisionals atomic.Int32
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
					t.Errorf("unexpected media path: %s", r.URL.Path)
					w.WriteHeader(http.StatusNotFound)
				}
			}))
			defer media.Close()
			s.media.server.controlURL = media.URL
			peer, err := net.ListenPacket("udp", "127.0.0.1:0")
			if err != nil {
				t.Fatal(err)
			}
			const deviceID = "34020000001320000001"
			const channelID = "34020000001320000002"
			host, portText, err := net.SplitHostPort(peer.LocalAddr().String())
			if err != nil {
				_ = peer.Close()
				t.Fatal(err)
			}
			port, err := strconv.Atoi(portText)
			if err != nil {
				_ = peer.Close()
				t.Fatal(err)
			}
			contact := sip.Uri{Scheme: "sip", User: deviceID, Host: host, Port: port}
			type packet struct {
				request *sip.Request
				address net.Addr
			}
			packets := make(chan packet, 32)
			peerErrors := make(chan error, 16)
			peerDone := make(chan struct{})
			report := func(err error) {
				select {
				case peerErrors <- err:
				default:
				}
			}
			respond := func(p packet, status int) {
				var body []byte
				if status == 200 && p.request.Method == sip.INVITE {
					body = bytes.ReplaceAll(p.request.Body(), []byte("a=recvonly"), []byte("a=sendonly"))
				}
				response := sip.NewResponseFromRequest(p.request, status, "Protocol Test", body)
				if status != 100 {
					response.To().Params.Add("tag", "invite-protocol-peer")
				}
				if body != nil {
					response.AppendHeader(&sip.ContactHeader{Address: contact})
					response.AppendHeader(sip.NewHeader("Content-Type", "application/sdp"))
				}
				if _, err := peer.WriteTo([]byte(response.String()), p.address); err != nil {
					report(err)
				}
				if status < 200 {
					provisionals.Add(1)
				}
			}
			go func() {
				defer close(peerDone)
				buffer := make([]byte, 65536)
				var invite *sip.Request
				var cancelIdentity string
				for {
					n, address, err := peer.ReadFrom(buffer)
					if err != nil {
						return
					}
					message, err := sip.ParseMessage(buffer[:n])
					if err != nil {
						report(err)
						continue
					}
					request, ok := message.(*sip.Request)
					if !ok {
						report(errors.New("unexpected SIP response at raw request peer"))
						continue
					}
					current := packet{request.Clone(), address}
					if request.Method == sip.INVITE {
						if invite != nil {
							if invite.Via().Value() != request.Via().Value() || invite.CSeq().SeqNo != request.CSeq().SeqNo {
								report(errors.New("unexpected second INVITE transaction"))
							}
							continue
						}
						invite = request.Clone()
					} else {
						if invite == nil || request.CallID() == nil || request.CSeq() == nil || request.Via() == nil ||
							request.From() == nil || request.To() == nil || request.CallID().Value() != invite.CallID().Value() ||
							request.From().Value() != invite.From().Value() {
							report(fmt.Errorf("invalid %s dialog identity", request.Method))
							continue
						}
						switch request.Method {
						case sip.CANCEL:
							cancelPackets.Add(1)
							identity := request.Via().Value() + "/" + request.CSeq().Value()
							if cancelIdentity != identity {
								cancelTransactions.Add(1)
								if cancelIdentity != "" {
									report(errors.New("duplicate CANCEL transaction"))
								}
								cancelIdentity = identity
							}
							if request.Via().Value() != invite.Via().Value() || request.CSeq().SeqNo != invite.CSeq().SeqNo ||
								request.To().Value() != invite.To().Value() || request.Recipient.String() != invite.Recipient.String() {
								report(errors.New("CANCEL changed original INVITE identity"))
							}
							respond(current, 200)
						case sip.ACK:
							acks.Add(1)
							tag, _ := request.To().Params.Get("tag")
							if request.CSeq().SeqNo != invite.CSeq().SeqNo || tag != "invite-protocol-peer" {
								report(errors.New("ACK changed INVITE CSeq or final response To tag"))
							}
							if !scenario.successful && (request.Via().Value() != invite.Via().Value() || request.Recipient.String() != invite.Recipient.String()) {
								report(errors.New("non-2xx ACK changed original INVITE Via or recipient"))
							}
						case sip.BYE:
							byes.Add(1)
							if acks.Load() == 0 || request.CSeq().SeqNo != invite.CSeq().SeqNo+1 {
								report(errors.New("BYE preceded ACK or changed expected dialog CSeq"))
							}
							respond(current, 200)
						default:
							report(fmt.Errorf("unexpected SIP method %s", request.Method))
						}
					}
					select {
					case packets <- current:
					default:
						report(errors.New("raw peer packet observation queue overflow"))
					}
				}
			}()
			defer func() {
				_ = peer.Close()
				select {
				case <-peerDone:
				case <-time.After(time.Second):
					t.Error("UDP peer goroutine did not exit")
				}
			}()
			awaitRequest := func(method sip.RequestMethod, timeout time.Duration) packet {
				t.Helper()
				timer := time.NewTimer(timeout)
				defer timer.Stop()
				for {
					select {
					case p := <-packets:
						if p.request.Method == method {
							return p
						}
						// 同一 CANCEL 事务的 UDP 重传不构成重复业务取消。
						if p.request.Method != sip.CANCEL || method != sip.ACK {
							t.Fatalf("received %s while waiting for %s", p.request.Method, method)
						}
					case err := <-peerErrors:
						t.Fatal(err)
					case <-timer.C:
						t.Fatalf("did not receive %s on UDP", method)
					}
				}
			}
			if err := s.live.sip.deviceStore.create(t.Context(), gbDevice{deviceID: deviceID, name: "invite protocol test"}); err != nil {
				t.Fatal(err)
			}
			s.live.sip.devices.register(registeredDevice{
				id: deviceID, contact: contact, remoteEndpoint: peer.LocalAddr().String(), online: true,
				expiresAt: time.Now().Add(time.Hour), lastHeartbeat: time.Now(),
			})
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
				select {
				case <-startDone:
				case <-time.After(2 * time.Second):
					t.Error("start goroutine did not exit after cancellation")
					// 仅失败收尾，不把 UA 关闭视作 ACK/BYE 协议成功。
					_ = s.live.sip.ua.Close()
					select {
					case <-startDone:
					case <-time.After(time.Second):
						t.Error("start goroutine remained after UA cleanup")
					}
				}
				if view, exists := s.live.live(deviceID, channelID); exists {
					t.Error("live remained during failure cleanup")
					cleanupContext, cleanupCancel := context.WithTimeout(context.Background(), time.Second)
					if err := s.live.stopLiveID(cleanupContext, view.streamID); err != nil {
						t.Errorf("failure cleanup: %v", err)
					}
					cleanupCancel()
				}
			}()
			invite := awaitRequest(sip.INVITE, time.Second)
			if scenario.successful {
				ticker := time.NewTicker(5 * time.Millisecond)
				defer ticker.Stop()
				for count := 0; count < 12; count++ {
					select {
					case <-ticker.C:
						respond(invite, 180)
					case got := <-completed:
						t.Fatalf("start returned before final response after %d provisionals: %v", count, got.err)
					case err := <-peerErrors:
						t.Fatal(err)
					}
				}
				ticker.Stop()
				respond(invite, 200)
				// 真实 UDP 突发最终响应；不宣称 OS 保证第二份先于首个 ACK 到达。
				respond(invite, 200)
			} else {
				if !scenario.early {
					respond(invite, 100)
				}
				cancel()
				cancel()
				cancel()
				if scenario.early {
					// 网络 barrier 已收到 INVITE；本窗口内 peer 明确没有发送 1xx。
					observation := time.NewTimer(100 * time.Millisecond)
					select {
					case p := <-packets:
						observation.Stop()
						t.Fatalf("sent %s before first provisional", p.request.Method)
					case got := <-completed:
						observation.Stop()
						t.Fatalf("start returned before late provisional: %v", got.err)
					case err := <-peerErrors:
						observation.Stop()
						t.Fatal(err)
					case <-observation.C:
					}
					if cancelPackets.Load() != 0 {
						t.Fatal("CANCEL observed before first provisional")
					}
					respond(invite, 100)
				}
				awaitRequest(sip.CANCEL, time.Second)
				cancel()
				respond(invite, 487)
			}
			var got result
			select {
			case got = <-completed:
			case <-time.After(2 * time.Second):
				t.Fatal("start did not return after final INVITE response")
			}
			awaitRequest(sip.ACK, time.Second)
			if scenario.successful {
				awaitRequest(sip.ACK, time.Second)
				if got.err != nil || got.view.state != liveStreaming || provisionals.Load() != 12 {
					t.Fatalf("success after twelve provisionals: view=%+v error=%v provisional=%d", got.view, got.err, provisionals.Load())
				}
				stopContext, stopCancel := context.WithTimeout(t.Context(), time.Second)
				err := s.live.stopLiveID(stopContext, got.view.streamID)
				stopCancel()
				if err != nil {
					t.Fatal(err)
				}
				awaitRequest(sip.BYE, time.Second)
			} else {
				var rejection sipgo.ErrDialogResponse
				if !errors.As(got.err, &rejection) || rejection.Res == nil || rejection.Res.StatusCode != 487 {
					t.Errorf("expected original INVITE 487 rejection, got %v", got.err)
				}
				// 本地 cleanup 后仍重传 487，证明事务保留 Timer D 并再次发非 2xx ACK。
				respond(invite, 487)
				// sipgo v1.6.0 actAckResend 明确延迟 T2，保留其真实计时器与 ERROR 日志。
				awaitRequest(sip.ACK, sip.T2+time.Second)
			}
			observation := time.NewTimer(100 * time.Millisecond)
		observe:
			for {
				select {
				case p := <-packets:
					// 同一 Via/CSeq 的 CANCEL 重传已经由 peer 校验，不算重复取消。
					if p.request.Method != sip.CANCEL {
						t.Errorf("unexpected extra SIP request after completion: %s", p.request.Method)
					}
				case err := <-peerErrors:
					t.Error(err)
				case <-observation.C:
					break observe
				}
			}
			s.live.mu.Lock()
			remaining := len(s.live.sessions)
			s.live.mu.Unlock()
			s.live.ssrcs.mu.Lock()
			allocated := len(s.live.ssrcs.active)
			s.live.ssrcs.mu.Unlock()
			wantCancel, wantBye := int32(1), int32(0)
			if scenario.successful {
				wantCancel, wantBye = 0, 1
			}
			if creates.Load() != 1 || deletes.Load() != 1 || remaining != 0 || allocated != 0 ||
				cancelTransactions.Load() != wantCancel || byes.Load() != wantBye || acks.Load() == 0 {
				t.Error("SIP obligations or local resource cleanup were not satisfied")
			}
			for {
				select {
				case err := <-peerErrors:
					t.Error(err)
				default:
					t.Logf("create=%d delete=%d provisional=%d CANCEL_packets=%d CANCEL_transactions=%d ACK=%d BYE=%d sessions=%d SSRC=%d",
						creates.Load(), deletes.Load(), provisionals.Load(), cancelPackets.Load(), cancelTransactions.Load(), acks.Load(), byes.Load(), remaining, allocated)
					return
				}
			}
		})
	}
}
