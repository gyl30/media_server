package main

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
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

func TestLiveCancelledInviteStillAcknowledgesSuccess(t *testing.T) {
	for _, scenario := range []struct {
		name         string
		cancelStatus int
		retransmit   bool
		recordRoute  bool
	}{
		{name: "normal"},
		{name: "cancel_200", cancelStatus: 200},
		{name: "cancel_481", cancelStatus: 481},
		{name: "cancel_unanswered", cancelStatus: -1},
		{name: "retransmitted_success_during_bye", retransmit: true},
		{name: "normal_record_route", recordRoute: true},
		{name: "record_route_retransmission", recordRoute: true, retransmit: true},
	} {
		t.Run(scenario.name, func(t *testing.T) {
			s := testInfrastructure(t)
			client, err := sipgo.NewClient(s.live.sip.ua,
				sipgo.WithClientAddr("127.0.0.1:0"), sipgo.WithClientConnectionAddr("127.0.0.1:0"))
			if err != nil {
				t.Fatal(err)
			}
			s.live.sip.client = client
			s.live.inviteTimeout = 5 * time.Second
			s.live.byeTimeout = time.Second
			var creates, deletes, invites, cancels, acks, byes atomic.Int32
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
						t.Error("deleted receiver does not match created receiver")
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
			const deviceID = "34020000001320000001"
			const channelID = "34020000001320000002"
			host, portText, _ := net.SplitHostPort(peer.LocalAddr().String())
			port, _ := strconv.Atoi(portText)
			contact := sip.Uri{Scheme: "sip", User: deviceID, Host: host, Port: port}
			type received struct {
				request *sip.Request
				address net.Addr
			}
			invited, byeReceived, peerDone := make(chan struct{}), make(chan struct{}), make(chan struct{})
			cancelReceived := make(chan received, 1)
			peerErrors := make(chan error, 8)
			respond := func(request *sip.Request, address net.Addr, status int, withSDP bool) {
				var body []byte
				if withSDP {
					body = bytes.ReplaceAll(request.Body(), []byte("a=recvonly"), []byte("a=sendonly"))
				}
				response := sip.NewResponseFromRequest(request, status, "Test", body)
				if status != 100 {
					response.To().Params.Add("tag", "cancelled-invite")
				}
				if withSDP {
					response.AppendHeader(&sip.ContactHeader{Address: contact})
					response.AppendHeader(sip.NewHeader("Content-Type", "application/sdp"))
					if scenario.recordRoute {
						response.AppendHeader(sip.NewHeader("Record-Route", "<"+contact.String()+";lr>"))
					}
				}
				if _, err := peer.WriteTo([]byte(response.String()), address); err != nil {
					peerErrors <- err
				}
			}
			go func() {
				defer close(peerDone)
				buffer := make([]byte, 65536)
				var invite, pendingBye received
				var firstBye *sip.Request
				for {
					n, address, err := peer.ReadFrom(buffer)
					if err != nil {
						return
					}
					message, err := sip.ParseMessage(buffer[:n])
					if err != nil {
						peerErrors <- err
						continue
					}
					request, ok := message.(*sip.Request)
					if !ok {
						continue
					}
					if request.Method == sip.ACK || request.Method == sip.BYE {
						if invite.request == nil || request.CallID() == nil || request.From() == nil || request.To() == nil || request.CSeq() == nil {
							peerErrors <- errors.New("incomplete ACK or BYE identity")
							continue
						}
						tag, _ := request.To().Params.Get("tag")
						if request.CallID().Value() != invite.request.CallID().Value() || request.From().Value() != invite.request.From().Value() || tag != "cancelled-invite" {
							peerErrors <- errors.New("ACK or BYE belongs to a different dialog")
							continue
						}
						if scenario.recordRoute && len(request.GetHeaders("Route")) != 1 {
							peerErrors <- errors.New("ACK or BYE does not preserve exactly one Route")
						}
					}
					switch request.Method {
					case sip.INVITE:
						if invites.Add(1) != 1 {
							continue
						}
						invite = received{request.Clone(), address}
						if scenario.cancelStatus == 0 {
							respond(request, address, 200, true)
						} else {
							respond(request, address, 100, false)
						}
						close(invited)
					case sip.CANCEL:
						first := cancels.Add(1) == 1
						if scenario.cancelStatus > 0 {
							respond(request, address, scenario.cancelStatus, false)
						}
						if first {
							// 已收到 CANCEL，但原 INVITE 同时成功，仍须 ACK 后 BYE。
							respond(invite.request, invite.address, 200, true)
							cancelReceived <- received{request.Clone(), address}
						}
					case sip.ACK:
						acks.Add(1)
						if request.CSeq().SeqNo != invite.request.CSeq().SeqNo {
							peerErrors <- errors.New("ACK CSeq differs from INVITE")
						}
						if pendingBye.request != nil {
							if scenario.recordRoute && acks.Load() == 2 {
								respond(invite.request, invite.address, 200, true)
								continue
							}
							respond(pendingBye.request, pendingBye.address, 200, false)
							pendingBye = received{}
						}
					case sip.BYE:
						if firstBye != nil && request.Via().Value() == firstBye.Via().Value() && request.CSeq().SeqNo == firstBye.CSeq().SeqNo {
							if pendingBye.request == nil {
								respond(request, address, 200, false)
							}
							continue
						}
						firstBye = request.Clone()
						if acks.Load() == 0 {
							peerErrors <- errors.New("BYE arrived before ACK")
						}
						if byes.Add(1) == 1 {
							close(byeReceived)
						}
						if scenario.retransmit {
							pendingBye = received{request.Clone(), address}
							respond(invite.request, invite.address, 200, true)
						} else {
							respond(request, address, 200, false)
						}
					}
				}
			}()
			defer func() { _ = peer.Close(); <-peerDone }()
			if err := s.live.sip.deviceStore.create(t.Context(), gbDevice{deviceID: deviceID, name: "cancel test"}); err != nil {
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
			defer cancel()
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
					return
				case <-time.After(time.Second):
					_ = s.live.sip.ua.Close()
				}
				select {
				case <-startDone:
				case <-time.After(time.Second):
					t.Error("start goroutine did not exit during test cleanup")
				}
			}()
			select {
			case <-invited:
			case <-time.After(2 * time.Second):
				t.Fatal("INVITE not received")
			}
			var cancellation received
			if scenario.cancelStatus != 0 {
				cancel()
				select {
				case cancellation = <-cancelReceived:
				case <-time.After(2 * time.Second):
					t.Fatal("CANCEL not received")
				}
			}
			var got result
			select {
			case got = <-completed:
			case <-time.After(2 * time.Second):
				t.Error("INVITE success not handled independently of CANCEL response")
				if scenario.cancelStatus == -1 {
					// 只在失败后的收尾补发，不能靠它让上面的有界断言通过。
					respond(cancellation.request, cancellation.address, 200, false)
				}
				select {
				case got = <-completed:
				case <-time.After(2 * time.Second):
					t.Fatal("start did not return after cleanup response")
				}
			}
			if scenario.cancelStatus == 0 {
				if got.err != nil {
					t.Fatal(got.err)
				}
				if err := s.live.stopLiveID(t.Context(), got.view.streamID); err != nil {
					t.Fatal(err)
				}
			} else if got.err == nil {
				t.Error("cancelled start returned success")
			}
			select {
			case <-byeReceived:
			case <-time.After(time.Second):
				t.Error("successful INVITE was not terminated by BYE")
			}
			s.live.mu.Lock()
			remaining := len(s.live.sessions)
			s.live.mu.Unlock()
			s.live.ssrcs.mu.Lock()
			allocated := len(s.live.ssrcs.active)
			s.live.ssrcs.mu.Unlock()
			t.Logf("receiver_create=%d delete=%d INVITE=%d CANCEL=%d ACK=%d BYE=%d sessions=%d SSRC=%d", creates.Load(), deletes.Load(), invites.Load(), cancels.Load(), acks.Load(), byes.Load(), remaining, allocated)
			if acks.Load() == 0 || byes.Load() != 1 || remaining != 0 || allocated != 0 || creates.Load() != 1 || deletes.Load() != 1 {
				t.Error("remote SIP and local cleanup obligations were not both satisfied")
			}
			if scenario.retransmit && acks.Load() < 2 {
				t.Error("INVITE 200 retransmission did not receive another ACK")
			}
			if scenario.recordRoute && scenario.retransmit && acks.Load() < 3 {
				t.Error("second INVITE 200 retransmission did not receive another ACK")
			}
			for {
				select {
				case err := <-peerErrors:
					t.Error(err)
				default:
					return
				}
			}
		})
	}
}
