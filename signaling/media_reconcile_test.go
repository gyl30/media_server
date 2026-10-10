package main

import (
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"sync"
	"sync/atomic"
	"testing"
	"time"
)

func TestPushReconcileUsesQueryStartForGrace(t *testing.T) {
	s := testInfrastructure(t)
	start := time.Now()
	var clock atomic.Int64
	clock.Store(start.UnixNano())
	s.live.sip.now = func() time.Time { return time.Unix(0, clock.Load()) }
	deviceID := createPushDevice(t, s)
	value := requestPushToken(t, s, deviceID)
	verifyToken(t, s, value, "publish", value, http.StatusOK)
	entered, release := make(chan struct{}), make(chan struct{})
	var queries atomic.Int32
	media := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
		if queries.Add(1) == 1 {
			close(entered)
			<-release
		}
		writeJSON(writer, http.StatusOK, map[string]any{"receivers": []mediaReceiver{}})
	}))
	t.Cleanup(media.Close)
	s.media.server.controlURL = media.URL
	done := make(chan error, 1)
	go func() { done <- s.reconcileMedia(t.Context()) }()
	select {
	case <-entered:
	case <-time.After(time.Second):
		close(release)
		t.Fatal("receiver query did not start")
	}
	clock.Store(start.Add(4 * time.Second).UnixNano())
	close(release)
	select {
	case err := <-done:
		if err != nil {
			t.Fatal(err)
		}
	case <-time.After(time.Second):
		t.Fatal("receiver query did not complete")
	}
	if _, ok := s.tokens.pushRuns[deviceID]; !ok {
		t.Fatal("query latency incorrectly exhausted registration grace")
	}
	if err := s.reconcileMedia(t.Context()); err != nil {
		t.Fatal(err)
	}
	if _, ok := s.tokens.pushRuns[deviceID]; ok {
		t.Fatal("subsequent query retained missing run beyond grace")
	}
}

func testStreamingLive(s *infrastructureServer, channelID, streamID string) *liveSession {
	established := make(chan struct{})
	close(established)
	session := &liveSession{
		key:      liveKey{deviceID: "34020000001320000001", channelID: channelID},
		streamID: streamID,
		state:    liveStreaming, cancel: func() {}, established: established, done: make(chan struct{}),
	}
	s.live.mu.Lock()
	s.live.sessions[session.key] = session
	s.live.mu.Unlock()
	return session
}

func TestMediaReconcileFollowsMediaServer(t *testing.T) {
	s := testInfrastructure(t)
	var mu sync.Mutex
	deleted := map[string]string{}
	media := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
		switch request.URL.Path {
		case "/receivers":
			writer.Header().Set("Content-Type", "application/json")
			_, _ = io.WriteString(writer, `{"receivers":[
				{"stream_id":"live-running"},
				{"stream_id":"pull-running"},
				{"stream_id":"live-orphan"},
				{"stream_id":"pull-orphan"}]}`)
		case "/receivers/delete":
			var body struct {
				StreamID string `json:"stream_id"`
			}
			decoder := json.NewDecoder(request.Body)
			decoder.DisallowUnknownFields()
			if err := decoder.Decode(&body); err != nil {
				t.Errorf("invalid receiver delete body: %v", err)
				writer.WriteHeader(http.StatusBadRequest)
				return
			}
			mu.Lock()
			deleted[body.StreamID] = request.URL.Path
			mu.Unlock()
			writer.WriteHeader(http.StatusNoContent)
		default:
			writer.WriteHeader(http.StatusNotFound)
		}
	}))
	t.Cleanup(media.Close)
	s.media.server.controlURL = media.URL

	running := testStreamingLive(s, "34020000001320000002", "live-running")
	ended := testStreamingLive(s, "34020000001320000003", "live-ended")
	s.rtspPulls["source-running"] = rtspPullSession{sourceID: "source-running", streamID: "pull-running", streamName: "rtsp/running", createConfirmed: true}
	s.rtspPulls["source-ended"] = rtspPullSession{sourceID: "source-ended", streamID: "pull-ended", streamName: "rtsp/ended", createConfirmed: true}
	s.rtspPulls["source-starting"] = rtspPullSession{sourceID: "source-starting", streamID: "pull-starting", streamName: "rtsp/starting", starting: true}

	if err := s.reconcileMedia(t.Context()); err != nil {
		t.Fatal(err)
	}

	if view, ok := s.live.live(running.key.deviceID, running.key.channelID); !ok || view.streamID != "live-running" {
		t.Fatal("running live was stopped")
	}
	if _, ok := s.live.live(ended.key.deviceID, ended.key.channelID); ok {
		t.Fatal("live ended on media server was retained")
	}
	if _, ok := s.rtspSourceSession("source-running"); !ok {
		t.Fatal("running rtsp pull was dropped")
	}
	if _, ok := s.rtspSourceSession("source-ended"); ok {
		t.Fatal("rtsp pull ended on media server was retained")
	}
	if _, ok := s.rtspSourceSession("source-starting"); !ok {
		t.Fatal("starting rtsp pull was dropped before create confirmed")
	}
	mu.Lock()
	defer mu.Unlock()
	if deleted["live-orphan"] != "/receivers/delete" || deleted["pull-orphan"] != "/receivers/delete" {
		t.Fatalf("orphan receivers not deleted: %v", deleted)
	}
	if _, ok := deleted["live-running"]; ok {
		t.Fatalf("running receiver deleted: %v", deleted)
	}
	if _, ok := deleted["pull-running"]; ok {
		t.Fatalf("running pull deleted: %v", deleted)
	}
}

func TestMediaReconcileKeepsStateWhenMediaUnavailable(t *testing.T) {
	s := testInfrastructure(t)
	media := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, _ *http.Request) {
		writer.WriteHeader(http.StatusInternalServerError)
	}))
	t.Cleanup(media.Close)
	s.media.server.controlURL = media.URL
	session := testStreamingLive(s, "34020000001320000002", "live-running")
	if err := s.reconcileMedia(t.Context()); err == nil {
		t.Fatal("unavailable media server accepted")
	}
	if _, ok := s.live.live(session.key.deviceID, session.key.channelID); !ok {
		t.Fatal("live stopped without media server evidence")
	}
}

func TestPushReconcileRegistrationGraceAndKnownIssuedRuns(t *testing.T) {
	s := testInfrastructure(t)
	now := time.Now()
	s.live.sip.now = func() time.Time { return now }
	recent, old, issued := createPushDevice(t, s), createPushDevice(t, s), createPushDevice(t, s)
	recentToken, oldToken, issuedToken := requestPushToken(t, s, recent), requestPushToken(t, s, old), requestPushToken(t, s, issued)
	verifyToken(t, s, recentToken, "publish", recentToken, http.StatusOK)
	verifyToken(t, s, oldToken, "publish", oldToken, http.StatusOK)
	s.tokens.mu.Lock()
	s.tokens.pushRuns[old] = pushRun{streamID: oldToken, verifiedAt: now.Add(-4 * time.Second)}
	s.tokens.mu.Unlock()
	play, err := s.newPlaybackURLs(playSource{PushDeviceID: old})
	if err != nil {
		t.Fatal(err)
	}
	var deleted []string
	media := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
		if request.URL.Path == "/receivers" {
			writeJSON(writer, http.StatusOK, map[string]any{"receivers": []map[string]string{
				{"stream_id": issuedToken}, {"stream_id": "orphan"},
			}})
			return
		}
		var command struct {
			StreamID string `json:"stream_id"`
		}
		if err := json.NewDecoder(request.Body).Decode(&command); err != nil {
			t.Error(err)
		}
		deleted = append(deleted, command.StreamID)
		writer.WriteHeader(http.StatusNoContent)
	}))
	t.Cleanup(media.Close)
	s.media.server.controlURL = media.URL
	if err := s.reconcileMedia(t.Context()); err != nil {
		t.Fatal(err)
	}
	if _, ok := s.tokens.pushRuns[recent]; !ok {
		t.Fatal("recent verification was released")
	}
	if _, ok := s.tokens.pushRuns[old]; ok {
		t.Fatal("missing established push run retained")
	}
	if _, ok := s.tokens.pushRuns[issued]; !ok {
		t.Fatal("unconsumed issued run was released")
	}
	if len(deleted) != 1 || deleted[0] != "orphan" {
		t.Fatalf("wrong orphan deletion: %v", deleted)
	}
	verifyToken(t, s, play.Token, "play", oldToken, http.StatusForbidden)
	now = now.Add(3 * time.Second)
	if err := s.reconcileMedia(t.Context()); err != nil {
		t.Fatal(err)
	}
	if _, ok := s.tokens.pushRuns[recent]; !ok {
		t.Fatal("exactly-three-second verification should remain in grace")
	}
	now = now.Add(time.Nanosecond)
	if err := s.reconcileMedia(t.Context()); err != nil {
		t.Fatal(err)
	}
	if _, ok := s.tokens.pushRuns[recent]; ok {
		t.Fatal("missing push beyond grace was retained")
	}
}

func TestPushReconcileQueryFailureAndStaleSnapshot(t *testing.T) {
	for _, scenario := range []string{"query failure", "new run"} {
		t.Run(scenario, func(t *testing.T) {
			s := testInfrastructure(t)
			now := time.Now()
			s.live.sip.now = func() time.Time { return now }
			deviceID := createPushDevice(t, s)
			old := requestPushToken(t, s, deviceID)
			verifyToken(t, s, old, "publish", old, http.StatusOK)
			s.tokens.pushRuns[deviceID] = pushRun{streamID: old, verifiedAt: now.Add(-4 * time.Second)}
			var next string
			media := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
				if scenario == "query failure" {
					writer.WriteHeader(http.StatusServiceUnavailable)
					return
				}
				next, _ = newStreamToken()
				s.tokens.mu.Lock()
				s.tokens.pushRuns[deviceID] = pushRun{streamID: next, verifiedAt: now}
				s.tokens.mu.Unlock()
				writeJSON(writer, http.StatusOK, map[string]any{"receivers": []mediaReceiver{}})
			}))
			t.Cleanup(media.Close)
			s.media.server.controlURL = media.URL
			err := s.reconcileMedia(t.Context())
			if scenario == "query failure" {
				if err == nil || s.tokens.pushRuns[deviceID].streamID != old {
					t.Fatal("failed query released run")
				}
			} else if err != nil || s.tokens.pushRuns[deviceID].streamID != next {
				t.Fatal("stale snapshot released replacement run")
			}
		})
	}
}
