package main

import (
	"bytes"
	"context"
	"fmt"
	"net"
	"net/http"
	"net/http/httptest"
	"net/url"
	"path"
	"strconv"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	"github.com/emiago/sipgo"
	"github.com/emiago/sipgo/sip"
)

// 真实 UDP SIP 对端；HTTP 只替代媒体控制，不把控制受理称作设备已暂停媒体。
type playbackControlPeer struct {
	t       *testing.T
	s       *infrastructureServer
	peer    net.PacketConn
	invite  *sip.Request
	answer  *sip.Response
	session *liveSession
	deletes *atomic.Int32
	seen    map[string]bool
	sender  net.Addr
}

func newPlaybackControlPeer(t *testing.T, purposes ...gbSession) *playbackControlPeer {
	t.Helper()
	s := testInfrastructure(t)
	deletes := new(atomic.Int32)
	media := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/gb28181/receiver/playback/create":
			writeJSON(w, http.StatusCreated, map[string]int{"rtp_port": 30001})
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
	t.Cleanup(media.Close)
	s.media.server.controlURL = media.URL
	peer, err := net.ListenPacket("udp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = peer.Close() })
	platform, err := net.ListenPacket("udp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	serverDone := make(chan error, 1)
	go func() { serverDone <- s.live.sip.server.ServeUDP(platform) }()
	client, err := sipgo.NewClient(s.live.sip.ua, sipgo.WithClientAddr("127.0.0.1:0"), sipgo.WithClientConnectionAddr("127.0.0.1:0"))
	if err != nil {
		t.Fatal(err)
	}
	s.live.sip.client = client
	host, portText, _ := net.SplitHostPort(peer.LocalAddr().String())
	port, _ := strconv.Atoi(portText)
	contact := sip.Uri{Scheme: "sip", User: gbPlaybackDevice, Host: host, Port: port}
	if err := s.live.sip.deviceStore.create(t.Context(), gbDevice{deviceID: gbPlaybackDevice, name: "playback control test"}); err != nil {
		t.Fatal(err)
	}
	s.live.sip.devices.register(registeredDevice{id: gbPlaybackDevice, contact: contact, remoteEndpoint: peer.LocalAddr().String(), online: true, expiresAt: time.Now().Add(time.Hour), lastHeartbeat: time.Now()})
	s.live.sip.channels.beginQuery(gbPlaybackDevice, 1)
	catalog := catalogResponse{DeviceID: gbPlaybackDevice, SN: 1, SumNum: 1}
	catalog.DeviceList.Num = 1
	catalog.DeviceList.Items = []catalogChannel{{DeviceID: gbPlaybackChannel, Status: "ON"}}
	if err := s.live.sip.channels.apply(catalog); err != nil {
		t.Fatal(err)
	}
	p := &playbackControlPeer{t: t, s: s, peer: peer, deletes: deletes, seen: make(map[string]bool)}
	t.Cleanup(func() {
		ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
		defer cancel()
		s.live.shutdown(ctx)
		_ = platform.Close()
		select {
		case <-serverDone:
		case <-time.After(2 * time.Second):
			t.Error("SIP server did not exit")
		}
	})
	purpose := sessionPlayback
	if len(purposes) != 0 {
		purpose = purposes[0]
	}
	ticket, err := s.startPlayback(t.Context(), gbPlaybackDevice, gbPlaybackChannel, 1700000000, 1700000010, purpose, 1)
	if err != nil {
		t.Fatal(err)
	}
	p.session = gbPlaybackFind(t, s, ticket.PlaybackID)
	parsed, err := url.Parse(ticket.PSURL)
	if err != nil {
		t.Fatal(err)
	}
	verifyToken(t, s, strings.TrimSuffix(path.Base(parsed.Path), ".ps"), "play", ticket.PlaybackID, http.StatusOK)
	p.invite = p.next(sip.INVITE)
	answerBody := bytes.ReplaceAll(p.invite.Body(), []byte("a=recvonly"), []byte("a=sendonly"))
	answerBody = bytes.ReplaceAll(answerBody, []byte("a=setup:passive"), []byte("a=setup:active"))
	p.answer = sip.NewResponseFromRequest(p.invite, sip.StatusOK, "OK", answerBody)
	p.answer.To().Params.Add("tag", "playback-control-peer")
	p.answer.AppendHeader(&sip.ContactHeader{Address: contact})
	p.answer.AppendHeader(sip.NewHeader("Content-Type", "application/sdp"))
	p.respond(p.answer)
	p.next(sip.ACK)
	gbPlaybackWait(t, p.session.established, "历史回放建立")
	s.live.mu.Lock()
	state := p.session.state
	s.live.mu.Unlock()
	if state != liveStreaming {
		t.Fatalf("历史回放未进入 streaming: %s", state)
	}
	return p
}

// next 读取下一条新请求；同一 branch 的 UDP 事务重传不是新的应用操作。
func (p *playbackControlPeer) next(method sip.RequestMethod) *sip.Request {
	p.t.Helper()
	buffer := make([]byte, 65536)
	for {
		_ = p.peer.SetReadDeadline(time.Now().Add(3 * time.Second))
		n, sender, err := p.peer.ReadFrom(buffer)
		if err != nil {
			p.t.Fatalf("等待 %s: %v", method, err)
		}
		message, err := sip.ParseMessage(buffer[:n])
		if err != nil {
			p.t.Fatal(err)
		}
		request, ok := message.(*sip.Request)
		if !ok {
			p.t.Fatalf("期望 %s 请求，收到 %v", method, message)
		}
		key := string(request.Method)
		if via := request.Via(); via != nil {
			key += via.Params.GetOr("branch", "")
		}
		if p.seen[key] {
			continue
		}
		p.seen[key] = true
		p.sender = sender
		if request.Method != method {
			p.t.Fatalf("期望 %s，收到 %s", method, request.Method)
		}
		return request
	}
}

func (p *playbackControlPeer) respond(response *sip.Response) {
	p.t.Helper()
	if _, err := p.peer.WriteTo([]byte(response.String()), p.sender); err != nil {
		p.t.Fatal(err)
	}
}

func (p *playbackControlPeer) control(action string, bodies ...string) <-chan *httptest.ResponseRecorder {
	result := make(chan *httptest.ResponseRecorder, 1)
	go func() {
		recorder := httptest.NewRecorder()
		body := ""
		if len(bodies) != 0 {
			body = bodies[0]
		}
		request := httptest.NewRequest(http.MethodPost, "/api/playbacks/"+p.session.streamID+"/"+action, strings.NewReader(body))
		if len(bodies) != 0 {
			request.Header.Set("Content-Type", "application/json")
		}
		p.s.handler().ServeHTTP(recorder, request)
		result <- recorder
	}()
	return result
}

func (p *playbackControlPeer) wait(result <-chan *httptest.ResponseRecorder, status int, code string) {
	p.t.Helper()
	select {
	case response := <-result:
		if response.Code != status || (code != "" && !strings.Contains(response.Body.String(), `"`+code+`"`)) {
			p.t.Fatalf("控制响应 HTTP %d %s，期望 %d %s", response.Code, response.Body.String(), status, code)
		}
	case <-time.After(8 * time.Second):
		p.t.Fatal("控制 HTTP 未返回")
	}
}

// checkInfo 核对 INFO 位于原对话、外层与内层 CSeq 一致，以及附录 B 请求体。
func (p *playbackControlPeer) checkInfo(info *sip.Request, sequence uint32, body string) {
	p.t.Helper()
	contentType := info.ContentType()
	if info.CallID() == nil || info.CallID().Value() != p.invite.CallID().Value() ||
		info.From().Params.GetOr("tag", "") != p.invite.From().Params.GetOr("tag", "") ||
		info.To().Params.GetOr("tag", "") != "playback-control-peer" ||
		info.CSeq().SeqNo != sequence || info.CSeq().MethodName != sip.INFO ||
		contentType == nil || contentType.Value() != "Application/MANSRTSP" || string(info.Body()) != body {
		p.t.Fatalf("INFO 不符合对话或附录 B: %s", info.String())
	}
}

func (p *playbackControlPeer) innerOK(info *sip.Request, sequence uint32) *sip.Response {
	return sip.NewResponseFromRequest(info, sip.StatusOK, "OK", []byte(fmt.Sprintf("RTSP/1.0 200 OK\r\nCSeq: %d\r\n\r\n", sequence)))
}

func TestGBPlaybackControlSequence(t *testing.T) {
	p := newPlaybackControlPeer(t)
	base := p.invite.CSeq().SeqNo
	p.wait(p.control("seek"), http.StatusBadRequest, "invalid_request")
	recorder := httptest.NewRecorder()
	p.s.handler().ServeHTTP(recorder, httptest.NewRequest(http.MethodPost, "/api/playbacks/00000000-0000-4000-8000-000000000000/pause", nil))
	if recorder.Code != http.StatusNotFound {
		t.Fatalf("未知实例 HTTP %d", recorder.Code)
	}

	pause := p.control("pause")
	info := p.next(sip.INFO)
	p.checkInfo(info, base+1, fmt.Sprintf("PAUSE RTSP/1.0\r\nCSeq: %d\r\nPauseTime: now\r\n\r\n", base+1))
	// 在途时第二个动作立即忙，不排队、不多发 INFO。
	p.wait(p.control("resume"), http.StatusConflict, "playback_control_busy")
	p.respond(p.innerOK(info, base+1))
	p.wait(pause, http.StatusNoContent, "")

	// 附录 J 示例的空体 200 也是受理。
	resume := p.control("resume")
	info = p.next(sip.INFO)
	p.checkInfo(info, base+2, fmt.Sprintf("PLAY RTSP/1.0\r\nCSeq: %d\r\nRange: npt=now-\r\n\r\n", base+2))
	p.respond(sip.NewResponseFromRequest(info, sip.StatusOK, "OK", nil))
	p.wait(resume, http.StatusNoContent, "")

	// 内层 CSeq 不符、内层非 200、外层拒绝都不能包装成成功，也不自动重发。
	for index, response := range []func(*sip.Request, uint32) *sip.Response{
		func(info *sip.Request, sequence uint32) *sip.Response {
			return sip.NewResponseFromRequest(info, sip.StatusOK, "OK", []byte(fmt.Sprintf("RTSP/1.0 200 OK\r\nCSeq: %d\r\n\r\n", sequence+7)))
		},
		func(info *sip.Request, sequence uint32) *sip.Response {
			return sip.NewResponseFromRequest(info, sip.StatusOK, "OK", []byte(fmt.Sprintf("RTSP/1.0 455 Method Not Valid in This State\r\nCSeq: %d\r\n\r\n", sequence)))
		},
		func(info *sip.Request, sequence uint32) *sip.Response {
			return sip.NewResponseFromRequest(info, sip.StatusOK, "OK", []byte(fmt.Sprintf("FOO 200 OK\r\nCSeq: %d\r\n\r\n", sequence)))
		},
		func(info *sip.Request, _ uint32) *sip.Response {
			return sip.NewResponseFromRequest(info, 488, "Not Acceptable Here", nil)
		},
	} {
		sequence := base + 3 + uint32(index)
		rejected := p.control("pause")
		info = p.next(sip.INFO)
		p.checkInfo(info, sequence, fmt.Sprintf("PAUSE RTSP/1.0\r\nCSeq: %d\r\nPauseTime: now\r\n\r\n", sequence))
		p.respond(response(info, sequence))
		p.wait(rejected, http.StatusBadGateway, "device_rejected")
	}

	// 没有响应时只结束本地等待；结果未知，不发送相反 INFO。
	started := time.Now()
	unknown := p.control("pause")
	p.next(sip.INFO)
	p.wait(unknown, http.StatusGatewayTimeout, "playback_control_failed")
	if elapsed := time.Since(started); elapsed < playbackControlTimeout-100*time.Millisecond {
		t.Fatalf("控制超时过早: %s", elapsed)
	}

	deleted := make(chan *httptest.ResponseRecorder, 1)
	go func() {
		recorder := httptest.NewRecorder()
		p.s.handler().ServeHTTP(recorder, httptest.NewRequest(http.MethodDelete, "/api/playbacks/"+p.session.streamID, nil))
		deleted <- recorder
	}()
	bye := p.next(sip.BYE)
	if bye.CSeq().SeqNo != base+8 {
		t.Fatalf("BYE CSeq=%d，期望 INFO 之后的 %d", bye.CSeq().SeqNo, base+8)
	}
	p.respond(sip.NewResponseFromRequest(bye, sip.StatusOK, "OK", nil))
	p.wait(deleted, http.StatusNoContent, "")
	p.wait(p.control("resume"), http.StatusNotFound, "playback_not_found")
	gbPlaybackResources(t, p.s, 0, 0, 0)
	if p.deletes.Load() != 1 {
		t.Fatalf("receiver delete=%d", p.deletes.Load())
	}
}

func TestGBPlaybackDeleteInterruptsControl(t *testing.T) {
	for _, command := range []struct{ action, body string }{{"pause", ""}, {"seek", `{"position":2.5}`}, {"scale", `{"scale":2}`}} {
		t.Run(command.action, func(t *testing.T) {
			p := newPlaybackControlPeer(t)
			base := p.invite.CSeq().SeqNo
			pause := p.control(command.action, command.body)
			p.next(sip.INFO)
			started := time.Now()
			deleted := make(chan *httptest.ResponseRecorder, 1)
			go func() {
				recorder := httptest.NewRecorder()
				p.s.handler().ServeHTTP(recorder, httptest.NewRequest(http.MethodDelete, "/api/playbacks/"+p.session.streamID, nil))
				deleted <- recorder
			}()
			// 显式停止取消在途等待（非超时，502）；BYE 仍排在已发 INFO 之后。
			p.wait(pause, http.StatusBadGateway, "playback_control_failed")
			bye := p.next(sip.BYE)
			if elapsed := time.Since(started); elapsed >= playbackControlTimeout {
				t.Fatalf("BYE 等待了完整 INFO 超时: %s", elapsed)
			}
			if bye.CSeq().SeqNo != base+2 {
				t.Fatalf("BYE CSeq=%d，期望 %d", bye.CSeq().SeqNo, base+2)
			}
			p.respond(sip.NewResponseFromRequest(bye, sip.StatusOK, "OK", nil))
			p.wait(deleted, http.StatusNoContent, "")
			p.wait(p.control("pause"), http.StatusNotFound, "playback_not_found")
			gbPlaybackResources(t, p.s, 0, 0, 0)
		})
	}
}

func TestGBPlaybackControlRequiresStreaming(t *testing.T) {
	s := testInfrastructure(t)
	media := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/gb28181/receiver/create":
			writeJSON(w, http.StatusCreated, map[string]int{"rtp_port": 30000})
		case "/gb28181/receiver/playback/create":
			writeJSON(w, http.StatusCreated, map[string]int{"rtp_port": 30001})
		case "/receivers/delete":
			w.WriteHeader(http.StatusNoContent)
		default:
			t.Errorf("unexpected media request %s", r.URL.Path)
			w.WriteHeader(http.StatusNotFound)
		}
	}))
	t.Cleanup(media.Close)
	s.media.server.controlURL = media.URL
	gbPlaybackPeer(t, s)
	ticket, _ := gbPlaybackCreate(t, s)
	recorder := apiRequest(t, s.handler(), http.MethodPost, "/api/playbacks/"+ticket.PlaybackID+"/pause", "", http.StatusConflict)
	if !strings.Contains(recorder.Body.String(), "playback_not_streaming") {
		t.Fatalf("未认领实例控制: %s", recorder.Body.String())
	}
	for _, command := range []struct{ action, body string }{{"seek", `{"position":1}`}, {"scale", `{"scale":2}`}} {
		apiRequest(t, s.handler(), http.MethodPost, "/api/playbacks/"+ticket.PlaybackID+"/"+command.action, command.body, http.StatusConflict)
	}
	// 普通实时 ID 不能走历史控制入口。
	view, err := s.live.startLive(t.Context(), gbPlaybackDevice, gbPlaybackChannel)
	if err != nil {
		t.Fatal(err)
	}
	apiRequest(t, s.handler(), http.MethodPost, "/api/playbacks/"+view.streamID+"/pause", "", http.StatusNotFound)
	apiRequest(t, s.handler(), http.MethodPost, "/api/playbacks/"+view.streamID+"/seek", `{"position":1}`, http.StatusNotFound)
}

func TestGBPlaybackRangeScaleSequence(t *testing.T) {
	p := newPlaybackControlPeer(t)
	base := p.invite.CSeq().SeqNo
	deadline, _ := p.session.ctx.Deadline()
	for index, command := range []struct{ action, body, header string }{
		{"seek", `{"position":0}`, "Range: npt=0-"},
		{"seek", `{"position":0.25}`, "Range: npt=0.25-"},
		{"seek", `{"position":10}`, "Range: npt=10-"},
		{"scale", `{"scale":0.25}`, "Scale: 0.25"},
		{"scale", `{"scale":0.5}`, "Scale: 0.5"},
		{"scale", `{"scale":1}`, "Scale: 1"},
		{"scale", `{"scale":2}`, "Scale: 2"},
		{"scale", `{"scale":4}`, "Scale: 4"},
	} {
		t.Run(command.header, func(t *testing.T) {
			result := p.control(command.action, command.body)
			info := p.next(sip.INFO)
			sequence := base + uint32(index) + 1
			p.checkInfo(info, sequence, fmt.Sprintf("PLAY RTSP/1.0\r\nCSeq: %d\r\n%s\r\n\r\n", sequence, command.header))
			p.wait(p.control("scale", `{"scale":1}`), http.StatusConflict, "playback_control_busy")
			p.respond(p.innerOK(info, sequence))
			p.wait(result, http.StatusNoContent, "")
		})
	}
	if after, _ := p.session.ctx.Deadline(); !after.Equal(deadline) {
		t.Fatalf("交互控制改变了原实例期限: %s -> %s", deadline, after)
	}
	gbPlaybackResources(t, p.s, 1, 0, 1)
	for index, action := range []string{"seek", "scale"} {
		body := `{"position":1}`
		if action == "scale" {
			body = `{"scale":2}`
		}
		result := p.control(action, body)
		info := p.next(sip.INFO)
		if info.CSeq().SeqNo != base+9+uint32(index) {
			t.Fatal("拒绝路径未延续原对话 CSeq")
		}
		if action == "seek" {
			p.respond(sip.NewResponseFromRequest(info, 488, "Not Acceptable Here", nil))
		} else {
			p.respond(p.innerOK(info, info.CSeq().SeqNo+1))
		}
		p.wait(result, http.StatusBadGateway, "device_rejected")
	}
}

func TestGBPlaybackRangeScaleValidation(t *testing.T) {
	p := newPlaybackControlPeer(t)
	for _, command := range []struct{ action, body string }{
		{"seek", ""}, {"seek", `{}`}, {"seek", `null`}, {"seek", `{"position":null}`},
		{"seek", `{"position":"1"}`}, {"seek", `{"position":true}`}, {"seek", `{"position":[]}`},
		{"seek", `{"position":-0.01}`}, {"seek", `{"position":10.01}`}, {"seek", `{"position":1e309}`},
		{"seek", `{"position":1,"scale":2}`}, {"seek", `{"position":1,"unknown":0}`}, {"seek", `{"position":1} {}`},
		{"scale", ""}, {"scale", `{}`}, {"scale", `null`}, {"scale", `{"scale":null}`},
		{"scale", `{"scale":"2"}`}, {"scale", `{"scale":true}`}, {"scale", `{"scale":[]}`},
		{"scale", `{"scale":0}`}, {"scale", `{"scale":-1}`}, {"scale", `{"scale":0.3}`},
		{"scale", `{"scale":8}`}, {"scale", `{"scale":1e309}`}, {"scale", `{"scale":2,"position":1}`},
		{"scale", `{"scale":2,"unknown":0}`}, {"scale", `{"scale":2} {}`},
	} {
		t.Run(command.action+command.body, func(t *testing.T) {
			p.wait(p.control(command.action, command.body), http.StatusBadRequest, "invalid_request")
		})
	}
	// 非法请求没有占用 CSeq 或留下 INFO；随后合法控制仍是原对话的第一条 INFO。
	result := p.control("seek", `{"position":-0}`)
	info := p.next(sip.INFO)
	p.checkInfo(info, p.invite.CSeq().SeqNo+1, fmt.Sprintf("PLAY RTSP/1.0\r\nCSeq: %d\r\nRange: npt=0-\r\n\r\n", p.invite.CSeq().SeqNo+1))
	p.respond(sip.NewResponseFromRequest(info, sip.StatusOK, "OK", nil))
	p.wait(result, http.StatusNoContent, "")
}

func TestGBPlaybackRangeScaleDownloadIsolation(t *testing.T) {
	p := newPlaybackControlPeer(t, sessionDownload)
	p.wait(p.control("seek", `{"position":1}`), http.StatusBadRequest, "invalid_request")
	p.wait(p.control("scale", `{"scale":2}`), http.StatusBadRequest, "invalid_request")
	// 不借新动作改变旧 pause/resume 对历史实例的已有行为。
	result := p.control("pause")
	info := p.next(sip.INFO)
	sequence := p.invite.CSeq().SeqNo + 1
	p.checkInfo(info, sequence, fmt.Sprintf("PAUSE RTSP/1.0\r\nCSeq: %d\r\nPauseTime: now\r\n\r\n", sequence))
	p.respond(p.innerOK(info, sequence))
	p.wait(result, http.StatusNoContent, "")
}

func TestGBPlaybackRangeScaleCancellation(t *testing.T) {
	p := newPlaybackControlPeer(t)
	ctx, cancel := context.WithCancel(t.Context())
	defer cancel()
	result := make(chan *httptest.ResponseRecorder, 1)
	go func() {
		recorder := httptest.NewRecorder()
		request := httptest.NewRequest(http.MethodPost, "/api/playbacks/"+p.session.streamID+"/seek", strings.NewReader(`{"position":2.5}`)).WithContext(ctx)
		request.Header.Set("Content-Type", "application/json")
		p.s.handler().ServeHTTP(recorder, request)
		result <- recorder
	}()
	p.next(sip.INFO)
	cancel()
	p.wait(result, http.StatusBadGateway, "playback_control_failed")
	// 取消只结束等待，不自动发送反向动作；停止仍在同一对话延续 CSeq。
	stopped := make(chan *httptest.ResponseRecorder, 1)
	go func() {
		recorder := httptest.NewRecorder()
		p.s.handler().ServeHTTP(recorder, httptest.NewRequest(http.MethodDelete, "/api/playbacks/"+p.session.streamID, nil))
		stopped <- recorder
	}()
	bye := p.next(sip.BYE)
	if bye.CSeq().SeqNo != p.invite.CSeq().SeqNo+2 {
		t.Fatal("取消后 BYE 未延续原 INFO CSeq")
	}
	p.respond(sip.NewResponseFromRequest(bye, sip.StatusOK, "OK", nil))
	p.wait(stopped, http.StatusNoContent, "")
	gbPlaybackResources(t, p.s, 0, 0, 0)
}

func TestGBPlaybackScaleTimeout(t *testing.T) {
	p := newPlaybackControlPeer(t)
	deadline, _ := p.session.ctx.Deadline()
	started := time.Now()
	result := p.control("scale", `{"scale":2}`)
	p.next(sip.INFO)
	p.wait(result, http.StatusGatewayTimeout, "playback_control_failed")
	if elapsed := time.Since(started); elapsed < playbackControlTimeout-100*time.Millisecond || elapsed > playbackControlTimeout+time.Second {
		t.Fatalf("Scale 未遵守总控制期限: %s", elapsed)
	}
	if after, _ := p.session.ctx.Deadline(); !after.Equal(deadline) {
		t.Fatal("Scale 超时改变了原实例期限")
	}
}
