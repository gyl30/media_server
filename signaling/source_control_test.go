package main

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"slices"
	"sync"
	"testing"
)

// fakePullMedia 记录媒体服务器收到的拉流创建与接收会话删除；fail 为真时拒绝创建。
type fakePullMedia struct {
	mu      sync.Mutex
	created []pullCreateRequest
	deleted []string
	fail    bool
	ready   bool
}

func (m *fakePullMedia) serve(t *testing.T, s *infrastructureServer) {
	server := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
		m.mu.Lock()
		defer m.mu.Unlock()
		switch request.URL.Path {
		case "/pull/create":
			var command pullCreateRequest
			if err := json.NewDecoder(request.Body).Decode(&command); err != nil {
				t.Error(err)
			}
			if m.fail {
				writeHTTPError(writer, http.StatusServiceUnavailable, "unavailable")
				return
			}
			m.created = append(m.created, command)
			writer.WriteHeader(http.StatusCreated)
		case "/receivers":
			receivers := make([]mediaReceiver, 0, len(m.created))
			for _, command := range m.created {
				if !slices.Contains(m.deleted, command.StreamID) {
					receivers = append(receivers, mediaReceiver{StreamID: command.StreamID, Ready: m.ready})
				}
			}
			writeJSON(writer, http.StatusOK, map[string]any{"receivers": receivers})
		case "/receivers/delete":
			var command struct {
				StreamID string `json:"stream_id"`
			}
			if err := json.NewDecoder(request.Body).Decode(&command); err != nil {
				t.Error(err)
			}
			m.deleted = append(m.deleted, command.StreamID)
			writer.WriteHeader(http.StatusNoContent)
		default:
			writer.WriteHeader(http.StatusNotFound)
		}
	}))
	t.Cleanup(server.Close)
	s.media.server.controlURL = server.URL
}

func listedSource(t *testing.T, handler http.Handler, sourceID string) sourceResponse {
	t.Helper()
	var list struct {
		Sources []sourceResponse `json:"sources"`
	}
	if err := json.Unmarshal(apiRequest(t, handler, "GET", "/api/sources", "", http.StatusOK).Body.Bytes(), &list); err != nil {
		t.Fatal(err)
	}
	for _, source := range list.Sources {
		if source.SourceID == sourceID {
			return source
		}
	}
	t.Fatalf("source %s not listed", sourceID)
	return sourceResponse{}
}

func decodeSource(t *testing.T, body []byte) sourceResponse {
	t.Helper()
	var source sourceResponse
	if err := json.Unmarshal(body, &source); err != nil {
		t.Fatalf("invalid source: %s", body)
	}
	return source
}

// 创建即拉流；媒体服务器拒绝时配置仍保留、期望状态为 running 且没有会话，重试成功后建立会话。
func TestSourceCreateStartsPullAndKeepsConfigOnFailure(t *testing.T) {
	s := testInfrastructure(t)
	media := &fakePullMedia{fail: true}
	media.serve(t, s)
	handler := s.handler()
	failed := decodeSource(t, apiRequest(t, handler, "POST", "/api/sources", `{"stream_name":"gate","url":"rtsp://10.0.0.1/live","username":"admin","password":"secret"}`, http.StatusCreated).Body.Bytes())
	if failed.DesiredState != sourceDesiredRunning || failed.Session != nil {
		t.Fatalf("failed start: %+v", failed)
	}
	listed := listedSource(t, handler, failed.SourceID)
	if listed.URL != "rtsp://10.0.0.1/live" || listed.Session != nil {
		t.Fatalf("config lost after failed start: %+v", listed)
	}
	media.fail = false
	apiRequest(t, handler, "POST", "/api/sources/"+failed.SourceID+"/start", "", http.StatusCreated)
	if current := listedSource(t, handler, failed.SourceID); current.Session == nil || current.Session.State != "created" {
		t.Fatalf("retry did not start: %+v", current)
	}
	if len(media.created) != 1 || media.created[0].URL != "rtsp://10.0.0.1/live" || media.created[0].Username == nil || *media.created[0].Password != "secret" {
		t.Fatalf("media creates: %+v", media.created)
	}

	// 媒体源是否已建立来自媒体对账：会话存在但轨道未就绪时为接入中。
	if err := s.reconcileMedia(t.Context()); err != nil {
		t.Fatal(err)
	}
	if current := listedSource(t, handler, failed.SourceID); current.Session.MediaReady {
		t.Fatalf("media ready before tracks: %+v", current)
	}
	media.ready = true
	if err := s.reconcileMedia(t.Context()); err != nil {
		t.Fatal(err)
	}
	if current := listedSource(t, handler, failed.SourceID); !current.Session.MediaReady {
		t.Fatalf("media not ready: %+v", current)
	}
}

// 修改连接参数会用新配置和新的 stream_id 重新拉流并删除旧会话；只改名称不重新拉流。
func TestSourcePatchRestartsRunningPull(t *testing.T) {
	s := testInfrastructure(t)
	media := &fakePullMedia{}
	media.serve(t, s)
	handler := s.handler()
	source := decodeSource(t, apiRequest(t, handler, "POST", "/api/sources", `{"stream_name":"gate","url":"rtsp://10.0.0.1/live"}`, http.StatusCreated).Body.Bytes())
	first := source.Session.StreamID
	renamed := decodeSource(t, apiRequest(t, handler, "PATCH", "/api/sources/"+source.SourceID, `{"stream_name":"gate 2"}`, http.StatusOK).Body.Bytes())
	if renamed.Session == nil || renamed.Session.StreamID != first || len(media.created) != 1 {
		t.Fatalf("rename restarted pull: %+v %d", renamed, len(media.created))
	}
	changed := decodeSource(t, apiRequest(t, handler, "PATCH", "/api/sources/"+source.SourceID, `{"url":"rtsp://10.0.0.2/live"}`, http.StatusOK).Body.Bytes())
	if changed.Session == nil || changed.Session.StreamID == first || changed.DesiredState != sourceDesiredRunning {
		t.Fatalf("url change did not restart: %+v", changed)
	}
	if len(media.created) != 2 || media.created[1].URL != "rtsp://10.0.0.2/live" || len(media.deleted) != 1 || media.deleted[0] != first {
		t.Fatalf("restart creates=%+v deletes=%v", media.created, media.deleted)
	}

	apiRequest(t, handler, "POST", "/api/sources/"+source.SourceID+"/stop", "", http.StatusNoContent)
	stopped := decodeSource(t, apiRequest(t, handler, "PATCH", "/api/sources/"+source.SourceID, `{"url":"rtsp://10.0.0.3/live"}`, http.StatusOK).Body.Bytes())
	if stopped.Session != nil || len(media.created) != 2 {
		t.Fatalf("stopped source restarted on patch: %+v", stopped)
	}
}

// 启动时只恢复期望运行的源，每个源一个会话，再次恢复不会重复建立。
func TestResumeSourcesFollowsDesiredState(t *testing.T) {
	path := filepath.Join(t.TempDir(), "signaling.db")
	store, err := openSourceStore(t.Context(), path)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = store.close() })
	for _, source := range []rtspSource{
		{sourceID: "11111111-1111-4111-8111-111111111111", streamName: "running", url: "rtsp://10.0.0.1/a", desiredState: sourceDesiredRunning},
		{sourceID: "22222222-2222-4222-8222-222222222222", streamName: "stopped", url: "rtsp://10.0.0.1/b", desiredState: sourceDesiredStopped},
	} {
		if err := store.create(t.Context(), source); err != nil {
			t.Fatal(err)
		}
	}
	s := testInfrastructure(t)
	s.sources = store
	media := &fakePullMedia{}
	media.serve(t, s)
	s.resumeSources(t.Context())
	s.resumeSources(t.Context())
	if len(media.created) != 1 || media.created[0].URL != "rtsp://10.0.0.1/a" {
		t.Fatalf("resumed creates: %+v", media.created)
	}
	if _, ok := s.rtspSourceSession("11111111-1111-4111-8111-111111111111"); !ok {
		t.Fatal("running source not resumed")
	}
	if _, ok := s.rtspSourceSession("22222222-2222-4222-8222-222222222222"); ok {
		t.Fatal("stopped source resumed")
	}
}

// RTMP 拉流来源与 RTSP 共用拉流源模型：需要 app 与流名，不接受用户名密码；创建即拉流。
func TestRTMPPullSourceValidationAndStart(t *testing.T) {
	s := testInfrastructure(t)
	media := &fakePullMedia{}
	media.serve(t, s)
	handler := s.handler()
	for _, body := range []string{
		`{"stream_name":"bad","url":"rtmp://10.0.0.1/live"}`,
		`{"stream_name":"bad","url":"rtmp://10.0.0.1//cam"}`,
		`{"stream_name":"bad","url":"rtmp://user:pass@10.0.0.1/live/cam"}`,
		`{"stream_name":"bad","url":"rtmp://10.0.0.1/live/cam","username":"admin","password":"x"}`,
		`{"stream_name":"bad","url":"srt://10.0.0.1/live/cam"}`,
	} {
		apiRequest(t, handler, "POST", "/api/sources", body, http.StatusBadRequest)
	}
	source := decodeSource(t, apiRequest(t, handler, "POST", "/api/sources", `{"stream_name":"cdn","url":"rtmp://10.0.0.1:1936/live/cam?key=abc"}`, http.StatusCreated).Body.Bytes())
	if source.Session == nil || len(media.created) != 1 || media.created[0].URL != "rtmp://10.0.0.1:1936/live/cam?key=abc" || media.created[0].Username != nil {
		t.Fatalf("rtmp source: %+v %+v", source, media.created)
	}
	apiRequest(t, handler, "PATCH", "/api/sources/"+source.SourceID, `{"username":"admin"}`, http.StatusBadRequest)
}
