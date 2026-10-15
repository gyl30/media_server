package main

import (
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

func TestLiveInviteAbsoluteDeadline(t *testing.T) {
	for _, scenario := range []string{
		"no_response",
		"only_100",
		"continuous_provisional",
	} {
		t.Run(scenario, func(t *testing.T) {
			s := testInfrastructure(t)
			client, err := sipgo.NewClient(s.live.sip.ua,
				sipgo.WithClientAddr("127.0.0.1:0"), sipgo.WithClientConnectionAddr("127.0.0.1:0"))
			if err != nil {
				t.Fatal(err)
			}
			s.live.sip.client = client
			const inviteLimit = 200 * time.Millisecond
			const compensationLimit = 100 * time.Millisecond
			s.live.inviteTimeout = inviteLimit
			s.live.byeTimeout = compensationLimit
			var creates, deletes, invitePackets, cancelPackets, cancelTransactions, acks, byes, provisionals atomic.Int32
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
			invited := make(chan packet, 1)
			unexpectedDialogRequest := make(chan sip.RequestMethod, 8)
			peerErrors := make(chan error, 16)
			peerDone := make(chan struct{})
			report := func(err error) {
				select {
				case peerErrors <- err:
				default:
				}
			}
			respond := func(p packet, status int) {
				response := sip.NewResponseFromRequest(p.request, status, "Deadline Test", nil)
				if status != 100 {
					response.To().Params.Add("tag", "invite-deadline-peer")
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
				var invite packet
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
						continue
					}
					current := packet{request.Clone(), address}
					if request.Method != sip.INVITE {
						if invite.request == nil || request.CallID() == nil || request.CSeq() == nil || request.Via() == nil ||
							request.CallID().Value() != invite.request.CallID().Value() {
							report(fmt.Errorf("invalid %s identity", request.Method))
							continue
						}
					}
					switch request.Method {
					case sip.INVITE:
						if invitePackets.Add(1) != 1 {
							continue
						}
						invite = current
						if scenario == "only_100" {
							respond(invite, 100)
						}
						invited <- packet{request.Clone(), address}
					case sip.CANCEL:
						cancelPackets.Add(1)
						identity := request.Via().Value() + "/" + request.CSeq().Value()
						if identity != cancelIdentity {
							cancelTransactions.Add(1)
							if cancelIdentity != "" {
								report(errors.New("more than one CANCEL transaction"))
							}
							cancelIdentity = identity
						}
						if request.CSeq().SeqNo != invite.request.CSeq().SeqNo || request.Via().Value() != invite.request.Via().Value() {
							report(errors.New("CANCEL does not reuse INVITE CSeq and Via"))
						}
						respond(current, 200)
					case sip.ACK:
						acks.Add(1)
						select {
						case unexpectedDialogRequest <- sip.ACK:
						default:
						}
					case sip.BYE:
						byes.Add(1)
						select {
						case unexpectedDialogRequest <- sip.BYE:
						default:
						}
						// 错误路径也答复，避免失败收尾等待完整 SIP 事务计时器。
						respond(current, 200)
					default:
						report(fmt.Errorf("unexpected SIP method %s", request.Method))
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
			if err := s.live.sip.deviceStore.create(t.Context(), gbDevice{deviceID: deviceID, name: "invite deadline test"}); err != nil {
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
					// 仅失败收尾；不以关闭 UA 代替上面的协议断言。
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
			var first packet
			select {
			case first = <-invited:
			case <-time.After(2 * time.Second):
				t.Fatal("INVITE not received")
			}
			started := time.Now()
			producerDone := make(chan struct{})
			if scenario == "continuous_provisional" {
				go func() {
					defer close(producerDone)
					ticker := time.NewTicker(5 * time.Millisecond)
					defer ticker.Stop()
					for {
						select {
						case <-startDone:
							return
						case <-ctx.Done():
							return
						case <-ticker.C:
							respond(first, 180)
						}
					}
				}()
			} else {
				close(producerDone)
			}
			defer func() {
				cancel()
				select {
				case <-producerDone:
				case <-time.After(time.Second):
					t.Error("provisional producer did not exit")
				}
			}()
			var got result
			// 持续发送 1xx 的 peer 不主动结束；只有绝对期限能使此断言通过。
			limit := time.NewTimer(inviteLimit + compensationLimit + 800*time.Millisecond)
			defer limit.Stop()
			select {
			case got = <-completed:
			case <-limit.C:
				t.Fatal("start exceeded absolute INVITE plus compensation deadline")
			}
			elapsed := time.Since(started)
			select {
			case <-producerDone:
			case <-time.After(time.Second):
				t.Fatal("provisional producer remained after start returned")
			}
			if !errors.Is(got.err, context.DeadlineExceeded) {
				t.Errorf("deadline result: %v", got.err)
			}
			// 跨过真实 UDP 重传周期，验证 Terminate 后不再发送 INVITE。
			completedInvites := invitePackets.Load()
			observation := time.NewTimer(sip.Timer_A + 100*time.Millisecond)
			select {
			case method := <-unexpectedDialogRequest:
				observation.Stop()
				t.Errorf("unexpected %s without final INVITE response", method)
			case <-observation.C:
			}
			if invitePackets.Load() != completedInvites {
				t.Errorf("INVITE retransmitted after start returned: before=%d after=%d", completedInvites, invitePackets.Load())
			}
			s.live.mu.Lock()
			remaining := len(s.live.sessions)
			s.live.mu.Unlock()
			s.live.ssrcs.mu.Lock()
			allocated := len(s.live.ssrcs.active)
			s.live.ssrcs.mu.Unlock()
			if creates.Load() != 1 || deletes.Load() != 1 || remaining != 0 || allocated != 0 {
				t.Errorf("cleanup create=%d delete=%d sessions=%d SSRC=%d", creates.Load(), deletes.Load(), remaining, allocated)
			}
			wantCancel := int32(1)
			if scenario == "no_response" {
				wantCancel = 0
			}
			if cancelTransactions.Load() != wantCancel || byes.Load() != 0 {
				t.Errorf("CANCEL transactions=%d want=%d; BYE packets=%d want=0", cancelTransactions.Load(), wantCancel, byes.Load())
			}
			if acks.Load() != 0 {
				t.Errorf("ACK sent without a final response: %d", acks.Load())
			}
			if scenario == "continuous_provisional" && provisionals.Load() < 12 {
				t.Errorf("did not exercise more than ten provisionals: %d", provisionals.Load())
			}
			for {
				select {
				case err := <-peerErrors:
					t.Error(err)
				default:
					t.Logf("elapsed=%s create=%d delete=%d INVITE_packets=%d provisional=%d CANCEL_packets=%d CANCEL_transactions=%d ACK=%d BYE=%d sessions=%d SSRC=%d", elapsed,
						creates.Load(), deletes.Load(), invitePackets.Load(), provisionals.Load(), cancelPackets.Load(), cancelTransactions.Load(), acks.Load(), byes.Load(), remaining, allocated)
					return
				}
			}
		})
	}
}
