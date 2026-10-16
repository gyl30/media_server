package main

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"net/http/httptest"
	"net/url"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"
)

const gbPlaybackDevice = "34020000001320000001"
const gbPlaybackChannel = "34020000001320000002"
const gbPlaybackPath = "/api/devices/" + gbPlaybackDevice + "/channels/" + gbPlaybackChannel + "/playback"
const gbPlaybackBody = `{"start_time":1760000000,"end_time":1760000010}`

func gbPlaybackWait(t *testing.T, done <-chan struct{}, step string) {
	t.Helper()
	select {
	case <-done:
	case <-time.After(3 * time.Second):
		t.Fatalf("等待%s超时", step)
	}
}

func gbPlaybackTicket(t *testing.T, response *httptest.ResponseRecorder) (playbackTicket, string) {
	t.Helper()
	var fields map[string]json.RawMessage
	var ticket playbackTicket
	if err := json.Unmarshal(response.Body.Bytes(), &fields); err != nil {
		t.Fatal(err)
	}
	if err := json.Unmarshal(response.Body.Bytes(), &ticket); err != nil {
		t.Fatal(err)
	}
	if response.Code != http.StatusCreated || len(fields) != 4 || !validUUIDv4(ticket.PlaybackID) || ticket.ExpiresAt.IsZero() || ticket.Deadline.IsZero() || response.Header().Get("Cache-Control") != "no-store" {
		t.Fatalf("历史票据不符合契约: HTTP %d %s", response.Code, response.Body.String())
	}
	parsed, err := url.Parse(ticket.PSURL)
	if err != nil {
		t.Fatal(err)
	}
	prefix := "/play/gb28181/" + ticket.PlaybackID + "/"
	token := strings.TrimSuffix(strings.TrimPrefix(parsed.Path, prefix), ".ps")
	if parsed.Scheme != "http" || parsed.Host == "" || !strings.HasPrefix(parsed.Path, prefix) || !strings.HasSuffix(parsed.Path, ".ps") || !validStreamToken(token) || parsed.RawQuery != "" {
		t.Fatalf("无效 PS URL: %s", ticket.PSURL)
	}
	return ticket, token
}

func gbPlaybackCreate(t *testing.T, s *infrastructureServer) (playbackTicket, string) {
	t.Helper()
	return gbPlaybackTicket(t, apiRequest(t, s.handler(), "POST", gbPlaybackPath, gbPlaybackBody, http.StatusCreated))
}

func gbPlaybackFind(t *testing.T, s *infrastructureServer, id string) *liveSession {
	t.Helper()
	s.live.mu.Lock()
	defer s.live.mu.Unlock()
	session := s.live.sessions[liveKey{deviceID: gbPlaybackDevice, channelID: gbPlaybackChannel, historyID: id}]
	if session == nil {
		t.Fatalf("历史 owner 不存在: %s", id)
	}
	return session
}

func gbPlaybackResources(t *testing.T, s *infrastructureServer, sessions, tokens, ssrcs int) {
	t.Helper()
	s.live.mu.Lock()
	actualSessions := len(s.live.sessions)
	s.live.mu.Unlock()
	s.tokens.mu.Lock()
	actualTokens := len(s.tokens.tokens)
	s.tokens.mu.Unlock()
	s.live.ssrcs.mu.Lock()
	actualSSRCs := len(s.live.ssrcs.active)
	s.live.ssrcs.mu.Unlock()
	if actualSessions != sessions || actualTokens != tokens || actualSSRCs != ssrcs {
		t.Fatalf("资源 sessions/tokens/SSRC=%d/%d/%d，期望%d/%d/%d", actualSessions, actualTokens, actualSSRCs, sessions, tokens, ssrcs)
	}
}

func gbPlaybackPeer(t *testing.T, s *infrastructureServer) (*atomic.Int32, <-chan []byte) {
	t.Helper()
	bodies := make(chan []byte, 16)
	invites := testLivePeerWithAnswer(t, s, gbPlaybackDevice, gbPlaybackChannel, func(answer []byte) []byte {
		bodies <- bytes.Clone(answer)
		return bytes.ReplaceAll(answer, []byte("a=setup:passive"), []byte("a=setup:active"))
	})
	return invites, bodies
}

func gbPlaybackNoInvite(t *testing.T, bodies <-chan []byte) {
	t.Helper()
	select {
	case body := <-bodies:
		t.Fatalf("claim 前不应 INVITE: %s", body)
	case <-time.After(60 * time.Millisecond):
	}
}

func TestGBPlaybackAPIPreconditions(t *testing.T) {
	s := testInfrastructure(t)
	var requests atomic.Int32
	media := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		requests.Add(1)
		w.WriteHeader(http.StatusInternalServerError)
	}))
	t.Cleanup(media.Close)
	s.media.server.controlURL = media.URL
	for _, body := range []string{
		`{}`, `{"start_time":0,"end_time":1}`, `{"start_time":10,"end_time":10}`,
		`{"start_time":10,"end_time":9}`, `{"start_time":"10","end_time":20}`,
		`{"start_time":1.5,"end_time":20}`, `{"start_time":1,"end_time":9223372036854775807}`,
		`{"start_time":1,"end_time":2,"speed":1}`, gbPlaybackBody + `{}`,
	} {
		apiRequest(t, s.handler(), "POST", gbPlaybackPath, body, http.StatusBadRequest)
	}
	apiRequest(t, s.handler(), "POST", "/api/devices/123/channels/"+gbPlaybackChannel+"/playback", gbPlaybackBody, http.StatusBadRequest)
	apiRequest(t, s.handler(), "POST", gbPlaybackPath, gbPlaybackBody, http.StatusNotFound)
	apiRequest(t, s.handler(), "POST", "/api/devices", `{"device_id":"`+gbPlaybackDevice+`","name":"历史摄像机"}`, http.StatusCreated)
	apiRequest(t, s.handler(), "POST", gbPlaybackPath, gbPlaybackBody, http.StatusConflict)
	apiRequest(t, s.handler(), "DELETE", "/api/playbacks/not-a-uuid", "", http.StatusBadRequest)
	apiRequest(t, s.handler(), "DELETE", "/api/playbacks/11111111-1111-4111-8111-111111111111", "", http.StatusNotFound)
	gbPlaybackResources(t, s, 0, 0, 0)
	if requests.Load() != 0 {
		t.Fatalf("前置拒绝不应请求媒体: %d", requests.Load())
	}
	t.Run("online_target", func(t *testing.T) {
		s := testInfrastructure(t)
		s.media.server.controlURL = media.URL
		invites, _ := gbPlaybackPeer(t, s)
		apiRequest(t, s.handler(), "POST", "/api/devices/"+gbPlaybackDevice+"/channels/34020000001320000003/playback", gbPlaybackBody, http.StatusNotFound)
		s.live.mu.Lock()
		s.live.stoppingDevices[gbPlaybackDevice] = struct{}{}
		s.live.mu.Unlock()
		apiRequest(t, s.handler(), "POST", gbPlaybackPath, gbPlaybackBody, http.StatusConflict)
		gbPlaybackResources(t, s, 0, 0, 0)
		if requests.Load() != 0 || invites.Load() != 0 {
			t.Fatalf("在线目标前置拒绝触发媒体或 SIP: %d/%d", requests.Load(), invites.Load())
		}
	})
}

func TestGBPlaybackPrepareAndClaimOnce(t *testing.T) {
	s := testInfrastructure(t)
	created, release := make(chan struct{}), make(chan struct{})
	var unblock sync.Once
	var receiver map[string]json.RawMessage
	var deletes atomic.Int32
	media := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/gb28181/receiver/playback/create":
			if r.Header.Get("Authorization") != "Bearer fixture-history-control" {
				t.Error("媒体控制缺少测试凭据")
			}
			if err := json.NewDecoder(r.Body).Decode(&receiver); err != nil {
				t.Error(err)
			}
			close(created)
			<-release
			writeJSON(w, http.StatusCreated, map[string]int{"rtp_port": 30001})
		case "/receivers/delete":
			deletes.Add(1)
			w.WriteHeader(http.StatusNoContent)
		default:
			t.Errorf("意外媒体请求 %s", r.URL.Path)
			w.WriteHeader(http.StatusNotFound)
		}
	}))
	t.Cleanup(media.Close)
	s.media.server.controlURL, s.media.server.controlToken = media.URL, "fixture-history-control"
	invites, bodies := gbPlaybackPeer(t, s)
	// 即使断言失败，也先解开 HTTP handler，再由已有 fixture 清理 owner。
	t.Cleanup(func() { unblock.Do(func() { close(release) }) })
	responses := make(chan *httptest.ResponseRecorder, 1)
	before := time.Now()
	go func() {
		r := httptest.NewRequest("POST", gbPlaybackPath, strings.NewReader(gbPlaybackBody))
		r.Header.Set("Content-Type", "application/json")
		w := httptest.NewRecorder()
		s.handler().ServeHTTP(w, r)
		responses <- w
	}()
	gbPlaybackWait(t, created, "媒体 create")
	gbPlaybackResources(t, s, 1, 0, 1)
	select {
	case <-responses:
		t.Fatal("receiver 尚未创建成功就签发票据")
	default:
	}
	gbPlaybackNoInvite(t, bodies)
	unblock.Do(func() { close(release) })
	var response *httptest.ResponseRecorder
	select {
	case response = <-responses:
	case <-time.After(3 * time.Second):
		t.Fatal("prepare 未完成")
	}
	ticket, token := gbPlaybackTicket(t, response)
	if len(receiver) != 4 || string(receiver["transport"]) != `"tcp_passive"` || string(receiver["payload_type"]) != "96" || string(receiver["stream_id"]) != fmt.Sprintf("%q", ticket.PlaybackID) {
		t.Fatalf("历史 create 必须恰好四字段: %v", receiver)
	}
	var allocated uint32
	if err := json.Unmarshal(receiver["ssrc"], &allocated); err != nil || allocated < 1000000000 {
		t.Fatalf("历史 SSRC: %s %v", receiver["ssrc"], err)
	}
	budget := 10*time.Second + s.tokenTTL + playbackCompletionGrace
	if ticket.Deadline.Before(before.Add(budget)) || ticket.Deadline.After(time.Now().Add(budget)) || ticket.ExpiresAt.Before(before.Add(s.tokenTTL)) || ticket.ExpiresAt.After(time.Now().Add(s.tokenTTL)) {
		t.Fatalf("建立预算或票据期限错误: %+v", ticket)
	}
	gbPlaybackResources(t, s, 1, 1, 1)
	gbPlaybackNoInvite(t, bodies)
	verifyToken(t, s, token, "play", "wrong-stream", http.StatusForbidden)
	verifyToken(t, s, token, "publish", ticket.PlaybackID, http.StatusForbidden)
	gbPlaybackResources(t, s, 1, 1, 1)
	results := make(chan int, 8)
	for range 8 {
		go func() {
			body := fmt.Sprintf(`{"token":%q,"operation":"play","stream_id":%q}`, token, ticket.PlaybackID)
			r := httptest.NewRequest("POST", "/internal/verify", strings.NewReader(body))
			r.RemoteAddr = "127.0.0.1:12345"
			r.Header.Set("Content-Type", "application/json")
			r.Header.Set("Authorization", "Bearer fixture-history-control")
			w := httptest.NewRecorder()
			s.handler().ServeHTTP(w, r)
			results <- w.Code
		}()
	}
	successes := 0
	for range 8 {
		select {
		case code := <-results:
			if code == http.StatusOK {
				successes++
			} else if code != http.StatusForbidden {
				t.Fatalf("并发 verify 意外 HTTP %d", code)
			}
		case <-time.After(3 * time.Second):
			t.Fatal("并发 verify 未结束")
		}
	}
	if successes != 1 {
		t.Fatalf("一次性 claim 成功数=%d", successes)
	}
	session := gbPlaybackFind(t, s, ticket.PlaybackID)
	gbPlaybackWait(t, session.established, "历史 INVITE")
	s.live.mu.Lock()
	state, port := session.state, session.rtpPort
	s.live.mu.Unlock()
	if state != liveStreaming || port != 30001 || invites.Load() != 1 {
		t.Fatalf("历史状态=%s port=%d INVITE=%d", state, port, invites.Load())
	}
	select {
	case body := <-bodies:
		for _, line := range []string{"s=Playback\r\n", "u=" + gbPlaybackChannel + ":0\r\n", "t=1760000000 1760000010\r\n", "m=video 30001 TCP/RTP/AVP 96\r\n", "a=setup:passive\r\n"} {
			if !strings.Contains(string(body), line) {
				t.Fatalf("历史 SDP 缺少 %q: %s", line, body)
			}
		}
	default:
		t.Fatal("未收到真实 UDP INVITE")
	}
	gbPlaybackResources(t, s, 1, 0, 1)
	apiRequest(t, s.handler(), "DELETE", "/api/playbacks/"+ticket.PlaybackID, "", http.StatusNoContent)
	gbPlaybackWait(t, session.done, "DELETE 回收")
	gbPlaybackResources(t, s, 0, 0, 0)
	if deletes.Load() != 1 {
		t.Fatalf("媒体 delete=%d", deletes.Load())
	}
	verifyToken(t, s, token, "play", ticket.PlaybackID, http.StatusForbidden)
}

func TestGBPlaybackIndependentFromLive(t *testing.T) {
	s := testInfrastructure(t)
	var mu sync.Mutex
	created := make(map[string]uint32)
	deleted := make(map[string]int)
	media := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		var command struct {
			StreamID string `json:"stream_id"`
			SSRC     uint32 `json:"ssrc"`
		}
		if err := json.NewDecoder(r.Body).Decode(&command); err != nil {
			t.Error(err)
		}
		mu.Lock()
		defer mu.Unlock()
		switch r.URL.Path {
		case "/gb28181/receiver/create", "/gb28181/receiver/playback/create":
			created[command.StreamID] = command.SSRC
			port := 30000
			if strings.Contains(r.URL.Path, "/playback/") {
				port = 30001
			}
			writeJSON(w, http.StatusCreated, map[string]int{"rtp_port": port})
		case "/receivers/delete":
			deleted[command.StreamID]++
			w.WriteHeader(http.StatusNoContent)
		default:
			t.Errorf("意外媒体请求 %s", r.URL.Path)
			w.WriteHeader(http.StatusNotFound)
		}
	}))
	t.Cleanup(media.Close)
	s.media.server.controlURL = media.URL
	invites, _ := gbPlaybackPeer(t, s)
	live := requestPlay(t, s.handler())
	type result struct {
		ticket playbackTicket
		err    error
	}
	results := make(chan result, 2)
	for range 2 {
		go func() {
			ticket, err := s.startPlayback(t.Context(), gbPlaybackDevice, gbPlaybackChannel, 1760000000, 1760000010, sessionPlayback, 1)
			results <- result{ticket, err}
		}()
	}
	tickets := make([]playbackTicket, 0, 2)
	for range 2 {
		select {
		case result := <-results:
			if result.err != nil {
				t.Fatal(result.err)
			}
			tickets = append(tickets, result.ticket)
		case <-time.After(3 * time.Second):
			t.Fatal("并发 prepare 超时")
		}
	}
	if tickets[0].PlaybackID == tickets[1].PlaybackID || tickets[0].PlaybackID == live.StreamID || tickets[1].PlaybackID == live.StreamID {
		t.Fatal("历史会话复用了实时或另一历史 identity")
	}
	gbPlaybackResources(t, s, 3, 3, 3)
	for _, ticket := range tickets {
		parsed, err := url.Parse(ticket.PSURL)
		if err != nil {
			t.Fatal(err)
		}
		token := strings.TrimSuffix(parsed.Path[strings.LastIndex(parsed.Path, "/")+1:], ".ps")
		verifyToken(t, s, token, "play", ticket.PlaybackID, http.StatusOK)
		gbPlaybackWait(t, gbPlaybackFind(t, s, ticket.PlaybackID).established, "独立历史 INVITE")
		apiRequest(t, s.handler(), "DELETE", "/api/lives/"+ticket.PlaybackID, "", http.StatusNotFound)
		apiRequest(t, s.handler(), "POST", "/api/play", `{"source":{"live_id":"`+ticket.PlaybackID+`"}}`, http.StatusConflict)
	}
	apiRequest(t, s.handler(), "DELETE", "/api/playbacks/"+live.StreamID, "", http.StatusNotFound)
	if view, exists := s.live.live(gbPlaybackDevice, gbPlaybackChannel); !exists || view.streamID != live.StreamID || invites.Load() != 3 {
		t.Fatalf("历史污染实时 identity: %+v exists=%t INVITE=%d", view, exists, invites.Load())
	}
	apiRequest(t, s.handler(), "DELETE", "/api/playbacks/"+tickets[0].PlaybackID, "", http.StatusNoContent)
	gbPlaybackResources(t, s, 2, 1, 2)
	apiRequest(t, s.handler(), "DELETE", "/api/lives/"+live.StreamID, "", http.StatusNoContent)
	gbPlaybackResources(t, s, 1, 0, 1)
	s.live.mu.Lock()
	remaining := s.live.sessions[liveKey{deviceID: gbPlaybackDevice, channelID: gbPlaybackChannel, historyID: tickets[1].PlaybackID}]
	if remaining == nil || remaining.state != liveStreaming {
		t.Error("删除另一会话影响剩余历史")
	}
	s.live.mu.Unlock()
	apiRequest(t, s.handler(), "DELETE", "/api/playbacks/"+tickets[1].PlaybackID, "", http.StatusNoContent)
	gbPlaybackResources(t, s, 0, 0, 0)
	mu.Lock()
	defer mu.Unlock()
	if len(created) != 3 || len(deleted) != 3 {
		t.Fatalf("create/delete=%v/%v", created, deleted)
	}
	suffixes := make(map[uint32]bool)
	for id, ssrc := range created {
		if deleted[id] != 1 || suffixes[ssrc%10000] || (id == live.StreamID) != (ssrc < 1000000000) {
			t.Fatalf("SSRC/资源隔离失败: %v/%v", created, deleted)
		}
		suffixes[ssrc%10000] = true
	}
}

func TestGBPlaybackTicketExpiry(t *testing.T) {
	for _, realTimer := range []bool{false, true} {
		t.Run(fmt.Sprintf("real_timer_%t", realTimer), func(t *testing.T) {
			s := testInfrastructure(t)
			var deletes atomic.Int32
			media := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				if r.URL.Path == "/gb28181/receiver/playback/create" {
					writeJSON(w, http.StatusCreated, map[string]int{"rtp_port": 30001})
				} else if r.URL.Path == "/receivers/delete" {
					deletes.Add(1)
					w.WriteHeader(http.StatusNoContent)
				} else {
					w.WriteHeader(http.StatusNotFound)
				}
			}))
			t.Cleanup(media.Close)
			s.media.server.controlURL = media.URL
			var clock atomic.Int64
			clock.Store(time.Now().UnixNano())
			s.live.sip.now = func() time.Time { return time.Unix(0, clock.Load()) }
			invites, _ := gbPlaybackPeer(t, s)
			if realTimer {
				s.tokenTTL = 200 * time.Millisecond
			}
			ticket, token := gbPlaybackCreate(t, s)
			session := gbPlaybackFind(t, s, ticket.PlaybackID)
			if realTimer {
				gbPlaybackWait(t, session.established, "未 claim 真实 TTL")
				gbPlaybackWait(t, session.done, "TTL 资源回收")
			} else {
				if !ticket.ExpiresAt.Equal(time.Unix(0, clock.Load()).Add(s.tokenTTL)) {
					t.Fatal("票据没有使用可控时钟")
				}
				clock.Store(ticket.ExpiresAt.UnixNano())
				verifyToken(t, s, token, "play", ticket.PlaybackID, http.StatusForbidden)
				apiRequest(t, s.handler(), "DELETE", "/api/playbacks/"+ticket.PlaybackID, "", http.StatusNoContent)
				gbPlaybackWait(t, session.established, "过期票据 DELETE")
			}
			verifyToken(t, s, token, "play", ticket.PlaybackID, http.StatusForbidden)
			gbPlaybackResources(t, s, 0, 0, 0)
			if invites.Load() != 0 || deletes.Load() != 1 {
				t.Fatalf("过期 INVITE/delete=%d/%d", invites.Load(), deletes.Load())
			}
		})
	}
}

func TestGBPlaybackPrepareFailureCleanup(t *testing.T) {
	for _, test := range []struct {
		name                string
		reject, deleteFails bool
	}{
		{"definitive_rejection", true, false}, {"ambiguous_compensated", false, false}, {"ambiguous_cleanup_pending", false, true},
	} {
		t.Run(test.name, func(t *testing.T) {
			s := testInfrastructure(t)
			var deletes atomic.Int32
			var failDelete atomic.Bool
			failDelete.Store(test.deleteFails)
			media := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				switch r.URL.Path {
				case "/gb28181/receiver/playback/create":
					if test.reject {
						writeHTTPError(w, http.StatusForbidden, "denied")
					} else {
						writeJSON(w, http.StatusCreated, map[string]int{"listen_port": 30001})
					}
				case "/receivers/delete":
					deletes.Add(1)
					if failDelete.Load() {
						w.WriteHeader(http.StatusServiceUnavailable)
					} else {
						w.WriteHeader(http.StatusNoContent)
					}
				case "/receivers":
					w.WriteHeader(http.StatusServiceUnavailable)
				default:
					t.Errorf("意外媒体请求 %s", r.URL.Path)
					w.WriteHeader(http.StatusNotFound)
				}
			}))
			t.Cleanup(media.Close)
			s.media.server.controlURL = media.URL
			invites, _ := gbPlaybackPeer(t, s)
			response := apiRequest(t, s.handler(), "POST", gbPlaybackPath, gbPlaybackBody, http.StatusBadGateway)
			var failure map[string]string
			if err := json.Unmarshal(response.Body.Bytes(), &failure); err != nil {
				t.Fatal(err)
			}
			if failure["error"] != "playback_start_failed" {
				t.Fatalf("错误响应: %v", failure)
			}
			wantDeletes := int32(1)
			if test.reject {
				wantDeletes = 0
			}
			if deletes.Load() != wantDeletes || invites.Load() != 0 {
				t.Fatalf("delete/INVITE=%d/%d", deletes.Load(), invites.Load())
			}
			if test.deleteFails {
				id := failure["playback_id"]
				if len(failure) != 2 || !validUUIDv4(id) {
					t.Fatalf("cleanup_pending 必须返回可回收 identity: %v", failure)
				}
				session := gbPlaybackFind(t, s, id)
				gbPlaybackWait(t, session.established, "失败 prepare owner")
				gbPlaybackResources(t, s, 1, 0, 1)
				// 到期只清理 Streaming；不能凭 deadline 忘记仍未确认删除的资源。
				expired, cancel := context.WithDeadline(context.Background(), time.Now().Add(-time.Second))
				s.live.mu.Lock()
				session.cancel()
				session.ctx, session.cancel = expired, cancel
				state := session.state
				s.live.mu.Unlock()
				if state != liveCleanupPending {
					t.Fatalf("失败事实状态=%s", state)
				}
				if err := s.reconcileMedia(t.Context()); err == nil {
					t.Fatal("媒体 list 故障应可见")
				}
				gbPlaybackResources(t, s, 1, 0, 1)
				if deletes.Load() != 1 {
					t.Fatal("deadline 对账不应自动重试 cleanup_pending")
				}
				failDelete.Store(false)
				apiRequest(t, s.handler(), "DELETE", "/api/playbacks/"+id, "", http.StatusNoContent)
			} else if len(failure) != 1 {
				t.Fatalf("已确认回收不应返回 playback_id: %v", failure)
			}
			gbPlaybackResources(t, s, 0, 0, 0)
		})
	}
}

func TestGBPlaybackDeadlineReconcile(t *testing.T) {
	s := testInfrastructure(t)
	var deletes atomic.Int32
	media := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/gb28181/receiver/playback/create":
			writeJSON(w, http.StatusCreated, map[string]int{"rtp_port": 30001})
		case "/receivers/delete":
			deletes.Add(1)
			w.WriteHeader(http.StatusNoContent)
		case "/receivers":
			w.WriteHeader(http.StatusServiceUnavailable)
		default:
			t.Errorf("意外媒体请求 %s", r.URL.Path)
			w.WriteHeader(http.StatusNotFound)
		}
	}))
	t.Cleanup(media.Close)
	s.media.server.controlURL = media.URL
	invites, _ := gbPlaybackPeer(t, s)
	ticket, token := gbPlaybackCreate(t, s)
	verifyToken(t, s, token, "play", ticket.PlaybackID, http.StatusOK)
	session := gbPlaybackFind(t, s, ticket.PlaybackID)
	gbPlaybackWait(t, session.established, "历史建立")
	// 已结束建立 goroutine 后，仅替换业务 deadline，避免等待真实录像时长。
	expired, cancel := context.WithDeadline(context.Background(), time.Now().Add(-time.Second))
	s.live.mu.Lock()
	state := session.state
	session.cancel()
	session.ctx, session.cancel = expired, cancel
	s.live.mu.Unlock()
	if state != liveStreaming {
		t.Fatalf("到期前必须实际 Streaming: %s", state)
	}
	if err := s.reconcileMedia(t.Context()); err == nil {
		t.Fatal("媒体 list 故障应可见")
	}
	gbPlaybackWait(t, session.done, "到期回收")
	gbPlaybackResources(t, s, 0, 0, 0)
	if invites.Load() != 1 || deletes.Load() != 1 {
		t.Fatalf("INVITE/delete=%d/%d", invites.Load(), deletes.Load())
	}
	apiRequest(t, s.handler(), "DELETE", "/api/playbacks/"+ticket.PlaybackID, "", http.StatusNotFound)
}

func TestGBPlaybackHTTPPrepareCancellation(t *testing.T) {
	s := testInfrastructure(t)
	entered, release := make(chan struct{}), make(chan struct{})
	var unblock sync.Once
	var creates, deletes atomic.Int32
	media := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/gb28181/receiver/playback/create":
			creates.Add(1)
			close(entered)
			<-release
			writeJSON(w, http.StatusCreated, map[string]int{"rtp_port": 30001})
		case "/receivers/delete":
			deletes.Add(1)
			w.WriteHeader(http.StatusNoContent)
		default:
			t.Errorf("意外媒体请求 %s", r.URL.Path)
			w.WriteHeader(http.StatusNotFound)
		}
	}))
	t.Cleanup(media.Close)
	s.media.server.controlURL = media.URL
	invites, _ := gbPlaybackPeer(t, s)
	finished := make(chan struct{})
	api := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		defer close(finished)
		s.handler().ServeHTTP(w, r)
	}))
	t.Cleanup(api.Close)
	t.Cleanup(func() { unblock.Do(func() { close(release) }) })
	ctx, cancel := context.WithCancel(t.Context())
	defer cancel()
	request, err := http.NewRequestWithContext(ctx, "POST", api.URL+gbPlaybackPath, strings.NewReader(gbPlaybackBody))
	if err != nil {
		t.Fatal(err)
	}
	request.Header.Set("Content-Type", "application/json")
	requestDone := make(chan error, 1)
	go func() {
		response, err := api.Client().Do(request)
		if response != nil {
			_ = response.Body.Close()
		}
		requestDone <- err
	}()
	gbPlaybackWait(t, entered, "阻塞 prepare")
	s.live.mu.Lock()
	var session *liveSession
	for _, candidate := range s.live.sessions {
		session = candidate
	}
	s.live.mu.Unlock()
	if session == nil {
		t.Fatal("阻塞 prepare 没有 owner")
	}
	gbPlaybackResources(t, s, 1, 0, 1)
	cancel()
	select {
	case err := <-requestDone:
		if !errors.Is(err, context.Canceled) {
			t.Fatalf("实际 HTTP 取消返回 %v", err)
		}
	case <-time.After(3 * time.Second):
		t.Fatal("HTTP 取消没有结束客户端")
	}
	unblock.Do(func() { close(release) })
	gbPlaybackWait(t, finished, "取消的 API handler")
	gbPlaybackWait(t, session.established, "取消的 prepare owner")
	gbPlaybackWait(t, session.done, "取消的资源回收")
	gbPlaybackResources(t, s, 0, 0, 0)
	if creates.Load() != 1 || deletes.Load() != 1 || invites.Load() != 0 {
		t.Fatalf("create/delete/INVITE=%d/%d/%d", creates.Load(), deletes.Load(), invites.Load())
	}
}

func TestGBPlaybackUnclaimedShutdown(t *testing.T) {
	s := testInfrastructure(t)
	var deletes atomic.Int32
	media := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/gb28181/receiver/playback/create" {
			writeJSON(w, http.StatusCreated, map[string]int{"rtp_port": 30001})
		} else if r.URL.Path == "/receivers/delete" {
			deletes.Add(1)
			w.WriteHeader(http.StatusNoContent)
		} else {
			w.WriteHeader(http.StatusNotFound)
		}
	}))
	t.Cleanup(media.Close)
	s.media.server.controlURL = media.URL
	invites, _ := gbPlaybackPeer(t, s)
	ticket, token := gbPlaybackCreate(t, s)
	session := gbPlaybackFind(t, s, ticket.PlaybackID)
	if s.tokenTTL != time.Minute {
		t.Fatalf("本场景必须保留真实60秒票期: %v", s.tokenTTL)
	}
	stopped := make(chan struct{})
	go func() { s.live.shutdown(t.Context()); close(stopped) }()
	gbPlaybackWait(t, stopped, "未 claim 停机，不等待票期")
	gbPlaybackWait(t, session.established, "停机 owner")
	gbPlaybackWait(t, session.done, "停机回收")
	gbPlaybackResources(t, s, 0, 0, 0)
	verifyToken(t, s, token, "play", ticket.PlaybackID, http.StatusForbidden)
	if invites.Load() != 0 || deletes.Load() != 1 {
		t.Fatalf("INVITE/delete=%d/%d", invites.Load(), deletes.Load())
	}
}

func TestGBPlaybackClaimConcurrentDelete(t *testing.T) {
	s := testInfrastructure(t)
	var deletes atomic.Int32
	media := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/gb28181/receiver/playback/create" {
			writeJSON(w, http.StatusCreated, map[string]int{"rtp_port": 30001})
		} else if r.URL.Path == "/receivers/delete" {
			deletes.Add(1)
			w.WriteHeader(http.StatusNoContent)
		} else {
			w.WriteHeader(http.StatusNotFound)
		}
	}))
	t.Cleanup(media.Close)
	s.media.server.controlURL = media.URL
	invites, _ := gbPlaybackPeer(t, s)
	ticket, token := gbPlaybackCreate(t, s)
	session := gbPlaybackFind(t, s, ticket.PlaybackID)
	start := make(chan struct{})
	verified, removed := make(chan int, 1), make(chan int, 1)
	go func() {
		<-start
		body := fmt.Sprintf(`{"token":%q,"operation":"play","stream_id":%q}`, token, ticket.PlaybackID)
		r := httptest.NewRequest("POST", "/internal/verify", strings.NewReader(body))
		r.RemoteAddr = "127.0.0.1:12345"
		r.Header.Set("Content-Type", "application/json")
		w := httptest.NewRecorder()
		s.handler().ServeHTTP(w, r)
		verified <- w.Code
	}()
	go func() {
		<-start
		w := httptest.NewRecorder()
		s.handler().ServeHTTP(w, httptest.NewRequest("DELETE", "/api/playbacks/"+ticket.PlaybackID, nil))
		removed <- w.Code
	}()
	close(start)
	var claimStatus int
	select {
	case claimStatus = <-verified:
		if claimStatus != http.StatusOK && claimStatus != http.StatusForbidden {
			t.Fatalf("竞争 claim HTTP %d", claimStatus)
		}
	case <-time.After(3 * time.Second):
		t.Fatal("竞争 claim 超时")
	}
	select {
	case status := <-removed:
		if status != http.StatusNoContent {
			t.Fatalf("竞争 DELETE HTTP %d", status)
		}
	case <-time.After(3 * time.Second):
		t.Fatal("竞争 DELETE 超时")
	}
	gbPlaybackWait(t, session.established, "竞争建立 owner")
	gbPlaybackWait(t, session.done, "竞争最终回收")
	gbPlaybackResources(t, s, 0, 0, 0)
	// claim 先赢时允许已排队的 INVITE；DELETE 先赢则不能启动 SIP。
	if deletes.Load() != 1 || invites.Load() > 1 || (claimStatus == http.StatusForbidden && invites.Load() != 0) {
		t.Fatalf("claim=%d INVITE/delete=%d/%d", claimStatus, invites.Load(), deletes.Load())
	}
	verifyToken(t, s, token, "play", ticket.PlaybackID, http.StatusForbidden)
}
