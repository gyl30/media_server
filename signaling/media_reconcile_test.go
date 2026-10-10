package main

import (
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"sync"
	"testing"
)

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
