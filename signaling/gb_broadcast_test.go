package main

import (
	"context"
	"encoding/json"
	"encoding/xml"
	"errors"
	"fmt"
	"net"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/emiago/sipgo"
	"github.com/emiago/sipgo/sip"
)

func TestBroadcastIDPersistence(t *testing.T) {
	path := filepath.Join(t.TempDir(), "broadcast.db")
	for round := range 2 {
		sources, err := openSourceStore(t.Context(), path)
		if err != nil {
			t.Fatal(err)
		}
		store, err := newDeviceStore(t.Context(), sources.db)
		if err != nil {
			t.Fatal(err)
		}
		var wait sync.WaitGroup
		ids := make(chan string, 8)
		for range 8 {
			wait.Go(func() {
				id, err := store.nextBroadcastID(t.Context(), "3402000000")
				if err != nil {
					t.Error(err)
				}
				ids <- id
			})
		}
		wait.Wait()
		close(ids)
		seen := make(map[string]bool)
		for id := range ids {
			if seen[id] {
				t.Errorf("reused %s", id)
			}
			seen[id] = true
		}
		for i := range 8 {
			id := fmt.Sprintf("3402000000136%07d", 2+round*8+i)
			if !seen[id] {
				t.Errorf("missing %s", id)
			}
		}
		if round == 1 {
			if _, err := sources.db.ExecContext(t.Context(), `UPDATE gb_broadcast_sequence SET last_value=9999999`); err != nil {
				t.Fatal(err)
			}
			if id, err := store.nextBroadcastID(t.Context(), "3402000000"); err == nil || id != "" {
				t.Fatalf("exhausted identity wrapped: %s %v", id, err)
			}
		}
		if err := sources.close(); err != nil {
			t.Fatal(err)
		}
	}
}

type broadcastWire struct {
	s          *infrastructureServer
	peer       net.PacketConn
	platform   net.Addr
	target     sip.Uri
	sequence   int
	mediaCalls chan talkMediaCall
}

type talkMediaCall struct {
	path string
	body map[string]any
}

type broadcastAttempt struct {
	cancel   context.CancelFunc
	results  chan broadcastResult
	sourceID string
	mediaID  string
	sn       int
	notify   *sip.Request
}

func (w *broadcastWire) start(t *testing.T) broadcastAttempt {
	t.Helper()
	ctx, cancel := context.WithCancel(t.Context())
	t.Cleanup(cancel)
	results := make(chan broadcastResult, 1)
	go func() {
		result, err := w.s.live.startBroadcast(ctx, queryTestDeviceID)
		result.err = err
		results <- result
	}()
	notify, ok := w.read(t).(*sip.Request)
	if !ok || notify.Method != sip.MESSAGE {
		t.Fatalf("missing Broadcast Notify: %v", notify)
	}
	var body struct {
		SN       int
		SourceID string
		TargetID string
	}
	if xml.Unmarshal(notify.Body(), &body) != nil || !validDigits(body.SourceID, 20) || body.TargetID != queryTestDeviceID {
		t.Fatalf("invalid Notify: %s", notify.Body())
	}
	w.s.live.mu.Lock()
	mediaID := w.s.live.broadcasts[queryTestDeviceID].mediaID
	w.s.live.mu.Unlock()
	return broadcastAttempt{cancel: cancel, results: results, sourceID: body.SourceID, mediaID: mediaID, sn: body.SN, notify: notify}
}

func newBroadcastWire(t *testing.T) *broadcastWire {
	t.Helper()
	s := testInfrastructure(t)
	platform, err := net.ListenPacket("udp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	client, err := sipgo.NewClient(s.live.sip.ua, sipgo.WithClientAddr(platform.LocalAddr().String()), sipgo.WithClientConnectionAddr(platform.LocalAddr().String()))
	if err != nil {
		t.Fatal(err)
	}
	s.live.sip.client = client
	serverDone := make(chan error, 1)
	go func() { serverDone <- s.live.sip.server.ServeUDP(platform) }()
	peer, err := net.ListenPacket("udp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	host, portText, _ := net.SplitHostPort(peer.LocalAddr().String())
	port, _ := strconv.Atoi(portText)
	w := &broadcastWire{s: s, peer: peer, platform: platform.LocalAddr(), target: sip.Uri{Scheme: "sip", User: queryTestDeviceID, Host: host, Port: port}, mediaCalls: make(chan talkMediaCall, 64)}
	media := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
		if request.Header.Get("Authorization") != "Bearer controlled-talk-media" {
			t.Error("missing media control credential")
			writeHTTPError(writer, http.StatusForbidden, "forbidden")
			return
		}
		if request.URL.Path == "/receivers" {
			writeJSON(writer, http.StatusOK, map[string]any{"receivers": []mediaReceiver{}})
			return
		}
		var body map[string]any
		if request.Method != http.MethodPost || json.NewDecoder(request.Body).Decode(&body) != nil {
			t.Error("invalid audio control request")
			writeHTTPError(writer, http.StatusBadRequest, "invalid_request")
			return
		}
		w.mediaCalls <- talkMediaCall{path: request.URL.Path, body: body}
		if request.URL.Path == "/gb28181/audio/create" {
			writeJSON(writer, http.StatusCreated, map[string]int{"rtp_port": 30000})
			return
		}
		writer.WriteHeader(http.StatusNoContent)
	}))
	t.Cleanup(media.Close)
	s.media.server.controlURL = media.URL
	s.media.server.controlToken = "controlled-talk-media"
	if err := s.live.sip.deviceStore.create(t.Context(), gbDevice{deviceID: queryTestDeviceID, name: "broadcast controlled peer"}); err != nil {
		t.Fatal(err)
	}
	s.live.sip.devices.register(registeredDevice{id: queryTestDeviceID, contact: w.target, remoteEndpoint: peer.LocalAddr().String(), online: true, expiresAt: time.Now().Add(time.Hour), lastHeartbeat: time.Now()})
	t.Cleanup(func() {
		ctx, cancel := context.WithTimeout(context.Background(), 8*time.Second)
		defer cancel()
		s.live.shutdown(ctx)
		_ = peer.Close()
		_ = platform.Close()
		<-serverDone
	})
	return w
}

func (w *broadcastWire) read(t *testing.T) sip.Message {
	t.Helper()
	_ = w.peer.SetReadDeadline(time.Now().Add(8 * time.Second))
	buffer := make([]byte, 65536)
	n, _, err := w.peer.ReadFrom(buffer)
	if err != nil {
		t.Fatal(err)
	}
	message, err := sip.ParseMessage(buffer[:n])
	if err != nil {
		t.Fatal(err)
	}
	return message
}

func (w *broadcastWire) send(t *testing.T, message sip.Message) {
	t.Helper()
	if _, err := w.peer.WriteTo([]byte(message.String()), w.platform); err != nil {
		t.Fatal(err)
	}
}

func (w *broadcastWire) request(method sip.RequestMethod, recipient string) *sip.Request {
	w.sequence++
	request := sip.NewRequest(method, sip.Uri{Scheme: "sip", User: recipient, Host: "127.0.0.1"})
	request.AppendHeader(sip.NewHeader("Via", fmt.Sprintf("SIP/2.0/UDP %s;branch=z9hG4bKbroadcast%d", w.peer.LocalAddr(), w.sequence)))
	params := sip.NewParams()
	params.Add("tag", "controlled-device")
	request.AppendHeader(&sip.FromHeader{Address: w.target, Params: params})
	request.AppendHeader(&sip.ToHeader{Address: request.Recipient})
	callID := sip.CallIDHeader(fmt.Sprintf("broadcast-%d", w.sequence))
	request.AppendHeader(&callID)
	request.AppendHeader(&sip.CSeqHeader{SeqNo: 1, MethodName: method})
	request.AppendHeader(&sip.ContactHeader{Address: w.target})
	maxForwards := sip.MaxForwardsHeader(70)
	request.AppendHeader(&maxForwards)
	request.SetBody(nil)
	return request
}

func (w *broadcastWire) business(t *testing.T, sn int, expected int) {
	t.Helper()
	request := w.request(sip.MESSAGE, w.s.live.sip.cfg.sipID)
	request.AppendHeader(sip.NewHeader("Content-Type", "Application/MANSCDP+xml"))
	request.SetBody([]byte(fmt.Sprintf("<Response><CmdType>Broadcast</CmdType><SN>%d</SN><DeviceID>%s</DeviceID><Result>OK</Result></Response>", sn, queryTestDeviceID)))
	w.send(t, request)
	response, ok := w.read(t).(*sip.Response)
	if !ok || response.StatusCode != expected {
		t.Fatalf("business response: %v", response)
	}
}

func (w *broadcastWire) invite(t *testing.T, sourceID string) (*sip.Request, *sip.Response) {
	t.Helper()
	request := w.request(sip.INVITE, sourceID)
	request.AppendHeader(sip.NewHeader("Subject", sourceID+":0,"+queryTestDeviceID+":non-decimal-sequence"))
	request.AppendHeader(sip.NewHeader("Content-Type", "application/sdp"))
	request.SetBody([]byte("v=0\r\no=device 0 0 IN IP4 127.0.0.1\r\ns=Play\r\nc=IN IP4 127.0.0.1\r\nt=0 0\r\nm=audio 20000 RTP/AVP 8 96\r\na=recvonly\r\na=rtpmap:8 PCMA/8000/1\r\ny=0123456789\r\n"))
	w.send(t, request)
	response, ok := w.read(t).(*sip.Response)
	if !ok || response.StatusCode != 200 {
		t.Fatalf("INVITE response: %v", response)
	}
	if !strings.Contains(string(response.Body()), "m=audio 30000 RTP/AVP 8") || !strings.Contains(string(response.Body()), "a=sendonly") {
		t.Fatalf("invalid SDP answer: %s", response.Body())
	}
	return request, response
}

func (w *broadcastWire) ack(t *testing.T, invite *sip.Request, answer *sip.Response, sameBranch bool, sequence uint32) {
	t.Helper()
	request := newLiveDialogRequest(invite, answer, sip.ACK)
	request.CSeq().SeqNo = sequence
	if sameBranch {
		request.AppendHeader(sip.HeaderClone(invite.Via()))
	} else {
		request.AppendHeader(sip.NewHeader("Via", "SIP/2.0/UDP "+w.peer.LocalAddr().String()+";branch=z9hG4bKnewack"))
	}
	w.send(t, request)
}

func TestBroadcastWireOrderingAndACK(t *testing.T) {
	for _, responseFirst := range []bool{true, false} {
		for _, sameBranch := range []bool{true, false} {
			t.Run(fmt.Sprintf("business-first=%v/same-branch=%v", responseFirst, sameBranch), func(t *testing.T) {
				w := newBroadcastWire(t)
				ctx, cancel := context.WithCancel(t.Context())
				defer cancel()
				results := make(chan broadcastResult, 1)
				go func() {
					result, err := w.s.live.startBroadcast(ctx, queryTestDeviceID)
					result.err = err
					results <- result
				}()
				notify, ok := w.read(t).(*sip.Request)
				if !ok || notify.Method != sip.MESSAGE {
					t.Fatalf("missing Broadcast Notify: %v", notify)
				}
				var body struct {
					SN       int
					SourceID string
					TargetID string
				}
				if xml.Unmarshal(notify.Body(), &body) != nil || body.SourceID != "34020000001360000002" || body.TargetID != queryTestDeviceID {
					t.Fatalf("invalid Notify: %s", notify.Body())
				}
				w.send(t, sip.NewResponseFromRequest(notify, 200, "OK", nil))
				w.business(t, body.SN+1, 403)
				if responseFirst {
					w.business(t, body.SN, 200)
				}
				invite, answer := w.invite(t, body.SourceID)
				w.ack(t, invite, answer, false, invite.CSeq().SeqNo+1)
				select {
				case result := <-results:
					t.Fatalf("authorized without real matching ACK: %+v", result)
				case <-time.After(25 * time.Millisecond):
				}
				w.ack(t, invite, answer, sameBranch, invite.CSeq().SeqNo)
				if !responseFirst {
					select {
					case result := <-results:
						t.Fatalf("authorized before business OK: %+v", result)
					case <-time.After(25 * time.Millisecond):
					}
					w.business(t, body.SN, 200)
				}
				var mediaID string
				select {
				case result := <-results:
					if result.err != nil || !validStreamToken(result.mediaID) || !validStreamToken(result.uploadToken) {
						t.Fatalf("broadcast result: %+v", result)
					}
					for _, path := range []string{"/gb28181/audio/create", "/gb28181/audio/configure"} {
						select {
						case call := <-w.mediaCalls:
							if call.path != path || call.body["session_id"] != result.mediaID {
								t.Fatalf("media ownership contract: %+v", call)
							}
							if path == "/gb28181/audio/configure" && (len(call.body) != 6 || call.body["upload_token"] != result.uploadToken ||
								call.body["remote_address"] != "127.0.0.1" || call.body["rtp_port"] != float64(20000) ||
								call.body["rtcp_port"] != float64(20001) || call.body["ssrc"] != float64(123456789)) {
								t.Fatalf("media peer configuration: %+v", call)
							}
						case <-time.After(time.Second):
							t.Fatalf("missing media request: %s", path)
						}
					}
					mediaID = result.mediaID
				case <-time.After(3 * time.Second):
					t.Fatal("matching ACK and business OK did not authorize")
				}
				if _, err := w.s.live.startBroadcast(ctx, queryTestDeviceID); !errors.Is(err, errBroadcastBusy) {
					t.Fatalf("concurrent broadcast: %v", err)
				}
				stopped := make(chan error, 1)
				go func() { stopped <- w.s.live.stopBroadcast(t.Context(), queryTestDeviceID, mediaID) }()
				bye, ok := w.read(t).(*sip.Request)
				if !ok || bye.Method != sip.BYE {
					t.Fatalf("missing local BYE: %v", bye)
				}
				w.send(t, sip.NewResponseFromRequest(bye, 200, "OK", nil))
				if err := <-stopped; err != nil {
					t.Fatal(err)
				}
				w.s.live.mu.Lock()
				remaining := len(w.s.live.broadcasts)
				w.s.live.mu.Unlock()
				if remaining != 0 {
					t.Fatal("broadcast owner remains after BYE")
				}
			})
		}
	}
}

func TestBroadcastCancellationAndDeviceFence(t *testing.T) {
	for _, action := range []string{"cancel-before-ACK", "remote-BYE-before-ACK", "remote-BYE", "offline", "delete", "shutdown"} {
		t.Run(action, func(t *testing.T) {
			w := newBroadcastWire(t)
			attempt := w.start(t)
			w.send(t, sip.NewResponseFromRequest(attempt.notify, 200, "OK", nil))
			w.business(t, attempt.sn, 200)
			invite, answer := w.invite(t, attempt.sourceID)
			beforeACK := strings.HasSuffix(action, "before-ACK")
			if !beforeACK {
				w.ack(t, invite, answer, false, invite.CSeq().SeqNo)
				select {
				case result := <-attempt.results:
					if result.err != nil {
						t.Fatal(result.err)
					}
				case <-time.After(3 * time.Second):
					t.Fatal("broadcast did not start")
				}
			}
			w.s.live.mu.Lock()
			session := w.s.live.broadcasts[queryTestDeviceID]
			w.s.live.mu.Unlock()
			if session == nil {
				t.Fatal("owner missing before stop")
			}
			finished := make(chan error, 1)
			switch action {
			case "cancel-before-ACK":
				attempt.cancel()
			case "offline":
				w.s.live.mu.Lock()
				sessions := w.s.live.beginDeviceStopLocked(queryTestDeviceID)
				w.s.live.mu.Unlock()
				go func() { w.s.live.finishDeviceOffline(queryTestDeviceID, sessions); finished <- nil }()
			case "delete":
				go func() { finished <- w.s.live.deleteDevice(t.Context(), queryTestDeviceID) }()
			case "shutdown":
				go func() { w.s.live.shutdown(t.Context()); finished <- nil }()
			default:
				bye := newLiveDialogRequest(invite, answer, sip.BYE)
				bye.CSeq().SeqNo++
				bye.AppendHeader(sip.NewHeader("Via", "SIP/2.0/UDP "+w.peer.LocalAddr().String()+";branch=z9hG4bKremotebye"))
				w.send(t, bye)
			}
			message := w.read(t)
			if response, ok := message.(*sip.Response); ok && action == "cancel-before-ACK" && response.StatusCode == 200 &&
				response.CallID().Value() == invite.CallID().Value() && response.CSeq().MethodName == sip.INVITE {
				// sipgo 的应答循环在 Terminate 后仍可能提交一次既有200，再返回事务错误。
				message = w.read(t)
			}
			if strings.HasPrefix(action, "remote-BYE") {
				response, ok := message.(*sip.Response)
				if !ok || response.StatusCode != 200 || response.CSeq().MethodName != sip.BYE {
					t.Fatalf("remote BYE response: %v", message)
				}
			} else {
				bye, ok := message.(*sip.Request)
				if !ok || bye.Method != sip.BYE {
					t.Fatalf("local BYE missing: %v", message)
				}
				if action == "offline" || action == "delete" {
					w.s.live.mu.Lock()
					_, fenced := w.s.live.stoppingDevices[queryTestDeviceID]
					retained := w.s.live.broadcasts[queryTestDeviceID] == session
					w.s.live.mu.Unlock()
					if !fenced || !retained {
						t.Fatal("device fence/owner released before BYE confirmation")
					}
				}
				w.send(t, sip.NewResponseFromRequest(bye, 200, "OK", nil))
			}
			select {
			case <-session.done:
			case <-time.After(8 * time.Second):
				t.Fatal("broadcast cleanup did not join")
			}
			if !beforeACK {
				if session.ctx.Err() == nil {
					t.Fatal("broadcast owner did not receive termination")
				}
				if action == "remote-BYE" && !errors.Is(context.Cause(session.ctx), errBroadcastRemoteBye) {
					t.Fatalf("remote end cause: %v", context.Cause(session.ctx))
				}
			}
			if beforeACK {
				select {
				case result := <-attempt.results:
					if result.err == nil {
						t.Fatal("canceled before ACK but authorized media")
					}
				case <-time.After(time.Second):
					t.Fatal("startup caller did not finish")
				}
			}
			if action == "offline" || action == "delete" || action == "shutdown" {
				if err := <-finished; err != nil {
					t.Fatal(err)
				}
			}
			if strings.HasPrefix(action, "remote-BYE") {
				_ = w.peer.SetReadDeadline(time.Now().Add(50 * time.Millisecond))
				if n, _, err := w.peer.ReadFrom(make([]byte, 65536)); err == nil {
					t.Fatalf("unexpected reverse BYE or packet after remote termination: %d bytes", n)
				}
			}
		})
	}
}

func TestBroadcastGenerationAndNotifyGate(t *testing.T) {
	w := newBroadcastWire(t)
	old := w.start(t)
	w.send(t, sip.NewResponseFromRequest(old.notify, 200, "OK", nil))
	old.cancel()
	if result := <-old.results; result.err == nil {
		t.Fatal("canceled notification authorized media")
	}
	current := w.start(t)
	if current.sourceID == old.sourceID || current.sn == old.sn {
		t.Fatal("broadcast identity reused")
	}
	w.business(t, old.sn, 403)
	stale := w.request(sip.INVITE, old.sourceID)
	stale.AppendHeader(sip.NewHeader("Subject", old.sourceID+":0,"+queryTestDeviceID+":0"))
	stale.AppendHeader(sip.NewHeader("Content-Type", "application/sdp"))
	stale.SetBody([]byte("v=0\r\n"))
	w.send(t, stale)
	if response, ok := w.read(t).(*sip.Response); !ok || response.StatusCode != 403 {
		t.Fatalf("old SourceID admitted: %v", response)
	}
	w.business(t, current.sn, 200)
	invite, answer := w.invite(t, current.sourceID)
	w.ack(t, invite, answer, false, invite.CSeq().SeqNo)
	select {
	case result := <-current.results:
		t.Fatalf("authorized before Notify transaction 2xx: %+v", result)
	case <-time.After(25 * time.Millisecond):
	}
	w.send(t, sip.NewResponseFromRequest(current.notify, 200, "OK", nil))
	select {
	case result := <-current.results:
		if result.err != nil {
			t.Fatal(result.err)
		}
	case <-time.After(3 * time.Second):
		t.Fatal("Notify 2xx did not finish startup")
	}
	if err := w.s.live.stopBroadcast(t.Context(), queryTestDeviceID, old.mediaID); !errors.Is(err, errBroadcastNotFound) {
		t.Fatalf("old stop selected a new owner: %v", err)
	}
	current.cancel()
	apiRequest(t, w.s.handler(), "GET", "/api/devices/"+queryTestDeviceID+"/talk", "", http.StatusOK)
	stopped := make(chan error, 1)
	go func() { stopped <- w.s.live.stopBroadcast(t.Context(), queryTestDeviceID, current.mediaID) }()
	bye, ok := w.read(t).(*sip.Request)
	if !ok || bye.Method != sip.BYE {
		t.Fatalf("current owner not retained: %v", bye)
	}
	w.send(t, sip.NewResponseFromRequest(bye, 200, "OK", nil))
	if err := <-stopped; err != nil {
		t.Fatal(err)
	}
}

func TestBroadcastDeleteCancelKeepsFence(t *testing.T) {
	w := newBroadcastWire(t)
	attempt := w.start(t)
	w.send(t, sip.NewResponseFromRequest(attempt.notify, 200, "OK", nil))
	w.business(t, attempt.sn, 200)
	invite, answer := w.invite(t, attempt.sourceID)
	w.ack(t, invite, answer, false, invite.CSeq().SeqNo)
	if result := <-attempt.results; result.err != nil {
		t.Fatal(result.err)
	}
	ctx, cancel := context.WithCancel(t.Context())
	defer cancel()
	deleted := make(chan error, 1)
	go func() { deleted <- w.s.live.deleteDevice(ctx, queryTestDeviceID) }()
	bye, ok := w.read(t).(*sip.Request)
	if !ok || bye.Method != sip.BYE {
		t.Fatalf("delete did not send BYE: %v", bye)
	}
	cancel()
	var err error
	returned := false
	select {
	case err = <-deleted:
		returned = true
	case <-time.After(100 * time.Millisecond):
	}
	w.s.live.mu.Lock()
	_, fenced := w.s.live.stoppingDevices[queryTestDeviceID]
	retained := w.s.live.broadcasts[queryTestDeviceID] != nil
	w.s.live.mu.Unlock()
	if !fenced || !retained {
		t.Errorf("delete cancellation released device fence before broadcast cleanup: fenced=%v owner=%v", fenced, retained)
	}
	w.send(t, sip.NewResponseFromRequest(bye, 200, "OK", nil))
	if !returned {
		err = <-deleted
	}
	if !errors.Is(err, context.Canceled) {
		t.Fatalf("canceled deletion: %v", err)
	}
}

func TestBroadcastRejectAndWrongDialog(t *testing.T) {
	for _, stage := range []string{"notify", "business", "wrong-dialog"} {
		t.Run(stage, func(t *testing.T) {
			w := newBroadcastWire(t)
			attempt := w.start(t)
			if stage == "notify" {
				w.send(t, sip.NewResponseFromRequest(attempt.notify, 500, "Failed", nil))
				if result := <-attempt.results; result.err == nil {
					t.Fatal("Notify failure authorized media")
				}
				return
			}
			w.send(t, sip.NewResponseFromRequest(attempt.notify, 200, "OK", nil))
			if stage == "business" {
				request := w.request(sip.MESSAGE, w.s.live.sip.cfg.sipID)
				request.AppendHeader(sip.NewHeader("Content-Type", "Application/MANSCDP+xml"))
				request.SetBody([]byte(fmt.Sprintf("<Response><CmdType>Broadcast</CmdType><SN>%d</SN><DeviceID>%s</DeviceID><Result>ERROR</Result></Response>", attempt.sn, queryTestDeviceID)))
				w.send(t, request)
				if response, ok := w.read(t).(*sip.Response); !ok || response.StatusCode != 200 {
					t.Fatalf("business error MESSAGE not acknowledged: %v", response)
				}
				if result := <-attempt.results; !errors.Is(result.err, errBroadcastRejected) {
					t.Fatalf("business error authorized media: %+v", result)
				}
				return
			}
			w.business(t, attempt.sn, 200)
			invite, answer := w.invite(t, attempt.sourceID)
			wrong := answer.Clone()
			wrong.To().Params.Add("tag", "another-dialog")
			w.ack(t, invite, wrong, false, invite.CSeq().SeqNo)
			select {
			case result := <-attempt.results:
				t.Fatalf("wrong dialog ACK authorized: %+v", result)
			case <-time.After(25 * time.Millisecond):
			}
			w.ack(t, invite, answer, false, invite.CSeq().SeqNo)
			if result := <-attempt.results; result.err != nil {
				t.Fatal(result.err)
			}
			bye := newLiveDialogRequest(invite, wrong, sip.BYE)
			bye.CSeq().SeqNo++
			bye.AppendHeader(sip.NewHeader("Via", "SIP/2.0/UDP "+w.peer.LocalAddr().String()+";branch=z9hG4bKwrongbye"))
			w.send(t, bye)
			if response, ok := w.read(t).(*sip.Response); !ok || response.StatusCode != 481 {
				t.Fatalf("wrong dialog BYE accepted: %v", response)
			}
			stopped := make(chan error, 1)
			go func() { stopped <- w.s.live.stopBroadcast(t.Context(), queryTestDeviceID, attempt.mediaID) }()
			localBye, ok := w.read(t).(*sip.Request)
			if !ok || localBye.Method != sip.BYE {
				t.Fatalf("wrong BYE removed owner: %v", localBye)
			}
			w.send(t, sip.NewResponseFromRequest(localBye, 200, "OK", nil))
			if err := <-stopped; err != nil {
				t.Fatal(err)
			}
		})
	}
}
