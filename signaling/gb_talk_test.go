package main

import (
	"context"
	"encoding/json"
	"encoding/xml"
	"io"
	"net/http"
	"net/http/httptest"
	"net/netip"
	"net/url"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/emiago/sipgo/sip"
)

func TestTalkHTTPRejectsInvalidRequest(t *testing.T) {
	s := testInfrastructure(t)
	handler := s.handler()
	apiRequest(t, handler, "POST", "/api/devices/invalid/talk", `{}`, http.StatusBadRequest)
	apiRequest(t, handler, "POST", "/api/devices/34020000001320000001/talk", `{"endpoint":"untrusted"}`, http.StatusBadRequest)
	apiRequest(t, handler, "POST", "/api/devices/34020000001320000001/talk", `{}`, http.StatusNotFound)
	apiRequest(t, handler, "DELETE", "/api/devices/34020000001320000001/talk", `{}`, http.StatusBadRequest)
	apiRequest(t, handler, "GET", "/api/devices/invalid/talk", ``, http.StatusBadRequest)
}

func TestTalkHTTPHandoffAndExactStop(t *testing.T) {
	for _, origin := range []string{"", "https://video.example.test:8443"} {
		t.Run(origin, func(t *testing.T) {
			w := newBroadcastWire(t)
			w.s.media.server.httpOrigin = origin
			path := "/api/devices/" + queryTestDeviceID + "/talk"
			ctx, cancel := context.WithCancel(t.Context())
			defer cancel()
			request := httptest.NewRequest("POST", path, strings.NewReader(`{}`)).WithContext(ctx)
			request.Header.Set("Content-Type", "application/json")
			response := httptest.NewRecorder()
			finished := make(chan struct{})
			go func() { defer close(finished); w.s.handler().ServeHTTP(response, request) }()
			notify, ok := w.read(t).(*sip.Request)
			if !ok || notify.Method != sip.MESSAGE {
				t.Fatalf("missing broadcast notification: %v", notify)
			}
			var message struct {
				SN       int
				SourceID string
			}
			if err := xml.Unmarshal(notify.Body(), &message); err != nil {
				t.Fatal(err)
			}
			create := <-w.mediaCalls
			id, _ := create.body["session_id"].(string)
			if create.path != "/gb28181/audio/create" || !validStreamToken(id) || len(create.body) != 1 {
				t.Fatalf("create contract: %+v", create)
			}
			apiRequest(t, w.s.handler(), "POST", path, `{}`, http.StatusConflict)
			apiRequest(t, w.s.handler(), "DELETE", path, `{"talk_id":"`+strings.Repeat("0", 64)+`"}`, http.StatusNotFound)
			w.send(t, sip.NewResponseFromRequest(notify, 200, "OK", nil))
			w.business(t, message.SN, 200)
			invite, answer := w.invite(t, message.SourceID)
			select {
			case <-finished:
				t.Fatal("public ticket returned before ACK")
			default:
			}
			w.ack(t, invite, answer, false, invite.CSeq().SeqNo)
			select {
			case <-finished:
			case <-time.After(3 * time.Second):
				t.Fatal("public ticket did not complete")
			}
			var ticket talkTicket
			if response.Code != http.StatusCreated || json.Unmarshal(response.Body.Bytes(), &ticket) != nil || ticket.TalkID != id || response.Header().Get("Cache-Control") != "no-store" {
				t.Fatalf("public ticket: %d %s", response.Code, response.Body.String())
			}
			upload, err := url.Parse(ticket.UploadURL)
			if err != nil || (origin == "" && upload.Scheme != "ws") || (origin != "" && !strings.HasPrefix(ticket.UploadURL, "ws"+strings.TrimPrefix(origin, "http")+"/")) || upload.RawQuery != "" || !strings.HasPrefix(upload.Path, "/publish/gb28181/audio/"+id+"/") {
				t.Fatalf("invalid upload URL: %v", err)
			}
			token := strings.TrimPrefix(upload.Path, "/publish/gb28181/audio/"+id+"/")
			if !validStreamToken(token) || token == id {
				t.Fatal("upload credential was not independent")
			}
			configured := <-w.mediaCalls
			if configured.path != "/gb28181/audio/configure" || len(configured.body) != 6 || configured.body["session_id"] != id || configured.body["upload_token"] != token ||
				configured.body["remote_address"] != "127.0.0.1" || configured.body["rtp_port"] != float64(20000) || configured.body["rtcp_port"] != float64(20001) || configured.body["ssrc"] != float64(123456789) {
				t.Fatalf("configure contract: %+v", configured)
			}
			cancel() // HTTP 请求成功后结束，不应取消已交付的 SIP owner。
			current := apiRequest(t, w.s.handler(), "GET", path, "", http.StatusOK)
			if strings.Contains(current.Body.String(), token) || strings.Contains(current.Body.String(), "upload_url") {
				t.Fatal("GET disclosed reusable upload credential")
			}
			if err := w.s.reconcileMedia(t.Context()); err != nil {
				t.Fatal(err)
			}
			if renewed := <-w.mediaCalls; renewed.path != "/gb28181/audio/status" || renewed.body["session_id"] != id {
				t.Fatalf("missing control renewal: %+v", renewed)
			}
			stopped := httptest.NewRecorder()
			done := make(chan struct{})
			go func() {
				defer close(done)
				deletion := httptest.NewRequest("DELETE", path, strings.NewReader(`{"talk_id":"`+id+`"}`))
				deletion.Header.Set("Content-Type", "application/json")
				w.s.handler().ServeHTTP(stopped, deletion)
			}()
			bye, ok := w.read(t).(*sip.Request)
			if !ok || bye.Method != sip.BYE {
				t.Fatalf("missing local BYE: %v", bye)
			}
			if deleted := <-w.mediaCalls; deleted.path != "/gb28181/audio/delete" || deleted.body["session_id"] != id {
				t.Fatalf("media delete: %+v", deleted)
			}
			w.send(t, sip.NewResponseFromRequest(bye, 200, "OK", nil))
			select {
			case <-done:
			case <-time.After(3 * time.Second):
				t.Fatal("public delete did not join")
			}
			if stopped.Code != http.StatusNoContent {
				t.Fatalf("public stop: %d %s", stopped.Code, stopped.Body.String())
			}
			apiRequest(t, w.s.handler(), "GET", path, "", http.StatusNotFound)
			apiRequest(t, w.s.handler(), "DELETE", path, `{"talk_id":"`+id+`"}`, http.StatusNotFound)
		})
	}
}

type failedTalkResponse struct{ header http.Header }

func (w failedTalkResponse) Header() http.Header       { return w.header }
func (w failedTalkResponse) WriteHeader(int)           {}
func (w failedTalkResponse) Write([]byte) (int, error) { return 0, io.ErrClosedPipe }

func TestTalkStartupFailureAndResponseLoss(t *testing.T) {
	for _, failure := range []string{"create-conflict", "create-invalid", "create-uncertain", "configure", "configure-cancel", "response-write"} {
		t.Run(failure, func(t *testing.T) {
			w := newBroadcastWire(t)
			var creates, configurations, deletes atomic.Int32
			configured, release := make(chan struct{}), make(chan struct{})
			var releaseOnce sync.Once
			media := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
				if request.Header.Get("Authorization") != "Bearer controlled-talk-media" {
					t.Error("missing control bearer")
				}
				switch request.URL.Path {
				case "/gb28181/audio/create":
					creates.Add(1)
					if failure == "create-conflict" {
						writeHTTPError(writer, http.StatusConflict, "session_conflict")
						return
					}
					if failure == "create-uncertain" {
						<-release
						writeJSON(writer, http.StatusCreated, map[string]int{"rtp_port": 30000})
						return
					}
					if failure == "create-invalid" {
						writeJSON(writer, http.StatusCreated, map[string]int{"rtp_port": 65535})
						return
					}
					writeJSON(writer, http.StatusCreated, map[string]int{"rtp_port": 30000})
				case "/gb28181/audio/configure":
					configurations.Add(1)
					if failure == "configure" {
						writeHTTPError(writer, http.StatusConflict, "configuration_failed")
						return
					}
					if failure == "configure-cancel" {
						close(configured)
						<-release
					}
					writer.WriteHeader(http.StatusNoContent)
				case "/gb28181/audio/delete":
					deletes.Add(1)
					writer.WriteHeader(http.StatusNoContent)
				default:
					writeHTTPError(writer, http.StatusNotFound, "not_found")
				}
			}))
			t.Cleanup(media.Close)
			t.Cleanup(func() { releaseOnce.Do(func() { close(release) }) })
			w.s.media.server.controlURL = media.URL
			ctx, cancel := context.WithCancel(t.Context())
			defer cancel()
			request := httptest.NewRequest("POST", "/api/devices/"+queryTestDeviceID+"/talk", strings.NewReader(`{}`)).WithContext(ctx)
			request.Header.Set("Content-Type", "application/json")
			recorded := httptest.NewRecorder()
			var writer http.ResponseWriter = recorded
			if failure == "response-write" {
				writer = failedTalkResponse{header: make(http.Header)}
			}
			done := make(chan struct{})
			go func() { defer close(done); w.s.handler().ServeHTTP(writer, request) }()
			if strings.HasPrefix(failure, "create-") {
				select {
				case <-done:
				case <-time.After(3 * time.Second):
					t.Fatal("create failure did not finish")
				}
				wantDelete := int32(1)
				if failure == "create-conflict" {
					wantDelete = 0
				}
				if recorded.Code != http.StatusBadGateway || creates.Load() != 1 || configurations.Load() != 0 || deletes.Load() != wantDelete {
					t.Fatalf("create failure ownership: code=%d create=%d configure=%d delete=%d", recorded.Code, creates.Load(), configurations.Load(), deletes.Load())
				}
				return
			}
			notify, ok := w.read(t).(*sip.Request)
			if !ok || notify.Method != sip.MESSAGE {
				t.Fatalf("missing Notify: %v", notify)
			}
			var body struct {
				SN       int
				SourceID string
			}
			if err := xml.Unmarshal(notify.Body(), &body); err != nil {
				t.Fatal(err)
			}
			w.send(t, sip.NewResponseFromRequest(notify, 200, "OK", nil))
			w.business(t, body.SN, 200)
			invite, answer := w.invite(t, body.SourceID)
			w.ack(t, invite, answer, false, invite.CSeq().SeqNo)
			if failure == "configure-cancel" {
				select {
				case <-configured:
				case <-time.After(3 * time.Second):
					t.Fatal("configure did not start")
				}
				cancel()
				releaseOnce.Do(func() { close(release) })
			}
			bye, ok := w.read(t).(*sip.Request)
			if !ok || bye.Method != sip.BYE {
				t.Fatalf("failure lost SIP cleanup: %v", bye)
			}
			w.send(t, sip.NewResponseFromRequest(bye, 200, "OK", nil))
			select {
			case <-done:
			case <-time.After(3 * time.Second):
				t.Fatal("failed startup did not join")
			}
			if creates.Load() != 1 || configurations.Load() != 1 || deletes.Load() != 1 {
				t.Fatalf("failed cleanup: %d %d %d", creates.Load(), configurations.Load(), deletes.Load())
			}
			if failure != "response-write" && recorded.Code != http.StatusBadGateway {
				t.Fatalf("failure returned success: %d", recorded.Code)
			}
			w.s.live.mu.Lock()
			remaining := len(w.s.live.broadcasts)
			w.s.live.mu.Unlock()
			if remaining != 0 {
				t.Fatal("failed owner remains")
			}
		})
	}
}

func TestTalkReconcileUsesConfirmedIdentity(t *testing.T) {
	for _, kind := range []string{"missing", "unknown", "replacement", "not-created"} {
		t.Run(kind, func(t *testing.T) {
			s := testInfrastructure(t)
			ctx, cancel := context.WithCancelCause(t.Context())
			defer cancel(context.Canceled)
			session := &broadcastSession{deviceID: queryTestDeviceID, mediaID: strings.Repeat("a", 64), ctx: ctx, cancel: cancel}
			if kind != "not-created" {
				session.local.rtp = netip.MustParseAddrPort("127.0.0.1:30000")
			}
			s.live.broadcasts[queryTestDeviceID] = session
			var statuses atomic.Int32
			media := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
				if request.URL.Path == "/gb28181/audio/status" {
					statuses.Add(1)
					if kind == "replacement" {
						s.live.mu.Lock()
						s.live.broadcasts[queryTestDeviceID] = &broadcastSession{deviceID: queryTestDeviceID, ctx: t.Context(), cancel: func(error) { t.Error("old status canceled replacement") }}
						s.live.mu.Unlock()
					}
					if kind == "unknown" {
						writeHTTPError(writer, http.StatusServiceUnavailable, "unavailable")
						return
					}
					writeHTTPError(writer, http.StatusNotFound, "not_found")
					return
				}
				// 音频对账必须先于这个视频查询失败。
				writeHTTPError(writer, http.StatusServiceUnavailable, "video_unavailable")
			}))
			t.Cleanup(media.Close)
			s.media.server.controlURL = media.URL
			if err := s.reconcileMedia(t.Context()); err == nil {
				t.Fatal("video query failure hidden")
			}
			if (ctx.Err() != nil) != (kind == "missing" || kind == "unknown") {
				t.Fatalf("wrong audio termination for %s: %v", kind, context.Cause(ctx))
			}
			want := int32(1)
			if kind == "not-created" {
				want = 0
			}
			if statuses.Load() != want {
				t.Fatalf("wrong status queries: %d", statuses.Load())
			}
		})
	}
}
