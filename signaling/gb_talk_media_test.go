package main

import (
	"bytes"
	"context"
	"encoding/binary"
	"encoding/json"
	"encoding/xml"
	"fmt"
	"net"
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/emiago/sipgo/sip"
	"github.com/gobwas/ws"
	"github.com/gobwas/ws/wsutil"
)

// 仅在显式指定本轮独立媒体服务时运行；默认 Go 单测不启动或触碰已有服务。
func TestTalkRealMediaUpload(t *testing.T) {
	controlURL := os.Getenv("GB_AUDIO_TEST_MEDIA_URL")
	if controlURL == "" {
		t.Skip("requires an isolated media_server with controlled-talk-media bearer")
	}
	address, err := url.Parse(controlURL)
	if err != nil || address.Scheme != "http" || address.Hostname() != "127.0.0.1" || address.RawQuery != "" || address.Path != "" {
		t.Fatal("test media must be an explicit isolated IPv4 loopback URL")
	}
	port, err := strconv.ParseUint(address.Port(), 10, 16)
	if err != nil || port == 0 {
		t.Fatal("explicit test HTTP port required")
	}
	for _, termination := range []string{"delete", "websocket-close", "remote-BYE"} {
		t.Run(termination, func(t *testing.T) {
			w := newBroadcastWire(t)
			w.s.media.server.controlURL = controlURL
			w.s.media.server.httpPort = uint16(port)
			rtp, err := net.ListenPacket("udp4", "127.0.0.1:0")
			if err != nil {
				t.Fatal(err)
			}
			t.Cleanup(func() { _ = rtp.Close() })
			rtcp, err := net.ListenPacket("udp4", "127.0.0.1:0")
			if err != nil {
				t.Fatal(err)
			}
			t.Cleanup(func() { _ = rtcp.Close() })
			path := "/api/devices/" + queryTestDeviceID + "/talk"
			response := httptest.NewRecorder()
			request := httptest.NewRequest("POST", path, strings.NewReader(`{}`))
			request.Header.Set("Content-Type", "application/json")
			ready := make(chan struct{})
			go func() { defer close(ready); w.s.handler().ServeHTTP(response, request) }()
			notify, ok := w.read(t).(*sip.Request)
			if !ok || notify.Method != sip.MESSAGE {
				t.Fatalf("missing Notify: %v", notify)
			}
			var business struct {
				SN       int
				SourceID string
			}
			if err := xml.Unmarshal(notify.Body(), &business); err != nil {
				t.Fatal(err)
			}
			w.send(t, sip.NewResponseFromRequest(notify, 200, "OK", nil))
			w.business(t, business.SN, 200)
			invite := w.request(sip.INVITE, business.SourceID)
			invite.AppendHeader(sip.NewHeader("Subject", business.SourceID+":0,"+queryTestDeviceID+":0"))
			invite.AppendHeader(sip.NewHeader("Content-Type", "application/sdp"))
			invite.SetBody([]byte(fmt.Sprintf("v=0\r\no=device 0 0 IN IP4 127.0.0.1\r\ns=Play\r\nc=IN IP4 127.0.0.1\r\nt=0 0\r\nm=audio %d RTP/AVP 8\r\na=recvonly\r\na=rtpmap:8 PCMA/8000/1\r\na=rtcp:%d\r\ny=0123456789\r\n", rtp.LocalAddr().(*net.UDPAddr).Port, rtcp.LocalAddr().(*net.UDPAddr).Port)))
			w.send(t, invite)
			answer, ok := w.read(t).(*sip.Response)
			if !ok || answer.StatusCode != 200 {
				t.Fatalf("real audio SIP answer: %v", answer)
			}
			w.ack(t, invite, answer, false, invite.CSeq().SeqNo)
			select {
			case <-ready:
			case <-time.After(3 * time.Second):
				t.Fatal("real media configure did not finish")
			}
			var ticket talkTicket
			if response.Code != http.StatusCreated || json.Unmarshal(response.Body.Bytes(), &ticket) != nil {
				t.Fatalf("real media ticket: %d %s", response.Code, response.Body.String())
			}
			ctx, cancel := context.WithTimeout(t.Context(), 3*time.Second)
			connection, _, _, err := (ws.Dialer{}).Dial(ctx, ticket.UploadURL)
			cancel()
			if err != nil {
				t.Fatal(err)
			}
			t.Cleanup(func() { _ = connection.Close() })
			_ = connection.SetWriteDeadline(time.Now().Add(3 * time.Second))
			var sequence uint16
			var timestamp uint32
			var sourcePort int
			for index := range 4 {
				samples := bytes.Repeat([]byte{byte(0x55 + index)}, 160)
				if err := wsutil.WriteClientBinary(connection, samples); err != nil {
					t.Fatal(err)
				}
				_ = rtp.SetReadDeadline(time.Now().Add(3 * time.Second))
				packet := make([]byte, 2048)
				count, source, err := rtp.ReadFrom(packet)
				if err != nil {
					t.Fatal(err)
				}
				if count != 172 || packet[0] != 0x80 || packet[1]&0x7f != 8 || binary.BigEndian.Uint32(packet[8:12]) != 123456789 || !bytes.Equal(packet[12:count], samples) {
					t.Fatal("SIP-configured real PCMA RTP mismatch")
				}
				currentSequence, currentTimestamp := binary.BigEndian.Uint16(packet[2:4]), binary.BigEndian.Uint32(packet[4:8])
				if index != 0 && (currentSequence != sequence+1 || currentTimestamp-timestamp != 160) {
					t.Fatal("real RTP order or audio clock changed")
				}
				sequence, timestamp = currentSequence, currentTimestamp
				sourcePort = source.(*net.UDPAddr).Port
			}
			if !strings.Contains(string(answer.Body()), fmt.Sprintf("m=audio %d RTP/AVP 8", sourcePort)) {
				t.Fatal("SIP answer did not advertise the bound media port")
			}
			_ = rtcp.SetReadDeadline(time.Now().Add(6 * time.Second))
			report := make([]byte, 2048)
			count, _, err := rtcp.ReadFrom(report)
			if err != nil || count < 28 || report[1] != 200 || binary.BigEndian.Uint32(report[4:8]) != 123456789 || binary.BigEndian.Uint32(report[20:24]) != 4 || binary.BigEndian.Uint32(report[24:28]) != 640 {
				t.Fatalf("real negotiated RTCP SR mismatch: bytes=%d error=%v", count, err)
			}
			// 一次性票据已由真正 C++ WS 认领，再次握手必须403。
			replayContext, cancelReplay := context.WithTimeout(t.Context(), 3*time.Second)
			replay, _, _, replayErr := (ws.Dialer{}).Dial(replayContext, ticket.UploadURL)
			cancelReplay()
			if replay != nil {
				_ = replay.Close()
			}
			if rejection, ok := replayErr.(ws.StatusError); !ok || int(rejection) != http.StatusForbidden {
				t.Fatalf("real credential replay not forbidden: %v", replayErr)
			}
			stopped := make(chan struct{})
			switch termination {
			case "delete":
				go func() {
					defer close(stopped)
					deletion := httptest.NewRequest("DELETE", path, strings.NewReader(`{"talk_id":"`+ticket.TalkID+`"}`))
					deletion.Header.Set("Content-Type", "application/json")
					reply := httptest.NewRecorder()
					w.s.handler().ServeHTTP(reply, deletion)
					if reply.Code != http.StatusNoContent {
						t.Errorf("real delete failed: HTTP %d", reply.Code)
					}
				}()
			case "websocket-close":
				_ = connection.Close()
				go func() {
					defer close(stopped)
					deadline := time.NewTimer(3 * time.Second)
					defer deadline.Stop()
					ticker := time.NewTicker(10 * time.Millisecond)
					defer ticker.Stop()
					for {
						select {
						case <-deadline.C:
							t.Error("WS close did not remove media identity")
							return
						case <-ticker.C:
							probe, cancel := context.WithTimeout(t.Context(), time.Second)
							err := w.s.media.post(probe, controlURL+"/gb28181/audio/status", map[string]string{"session_id": ticket.TalkID}, http.StatusNoContent, nil)
							cancel()
							if isMediaServerNotFound(err) {
								_ = w.s.reconcileMedia(t.Context())
								return
							}
						}
					}
				}()
			case "remote-BYE":
				bye := newLiveDialogRequest(invite, answer, sip.BYE)
				bye.CSeq().SeqNo++
				bye.AppendHeader(sip.NewHeader("Via", "SIP/2.0/UDP "+w.peer.LocalAddr().String()+";branch=z9hG4bKrealaudiobye"))
				w.send(t, bye)
				close(stopped)
			}
			message := w.read(t)
			if termination == "remote-BYE" {
				if reply, ok := message.(*sip.Response); !ok || reply.StatusCode != 200 || reply.CSeq().MethodName != sip.BYE {
					t.Fatalf("remote BYE: %v", message)
				}
			} else {
				bye, ok := message.(*sip.Request)
				if !ok || bye.Method != sip.BYE {
					t.Fatalf("missing real local BYE: %v", message)
				}
				w.send(t, sip.NewResponseFromRequest(bye, 200, "OK", nil))
			}
			select {
			case <-stopped:
			case <-time.After(3 * time.Second):
				t.Fatal("real stop did not return")
			}
			// 以 owner done join，不把网络204直接当成双方清理完成。
			w.s.live.mu.Lock()
			owner := w.s.live.broadcasts[queryTestDeviceID]
			w.s.live.mu.Unlock()
			if owner != nil {
				select {
				case <-owner.done:
				case <-time.After(3 * time.Second):
					t.Fatal("real owner retained")
				}
			}
			probe, cancel := context.WithTimeout(t.Context(), time.Second)
			defer cancel()
			if err := w.s.media.post(probe, controlURL+"/gb28181/audio/status", map[string]string{"session_id": ticket.TalkID}, http.StatusNoContent, nil); !isMediaServerNotFound(err) {
				t.Fatalf("real media identity remains: %v", err)
			}
			apiRequest(t, w.s.handler(), "GET", path, "", http.StatusNotFound)
		})
	}
}
