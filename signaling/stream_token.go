package main

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"errors"
	"maps"
	"net"
	"net/http"
	"net/url"
	"strconv"
	"sync"
	"time"
)

type streamToken struct {
	streamID  string
	expiresAt time.Time
}

type pushRun struct {
	streamID   string
	verifiedAt time.Time
}

type streamTokens struct {
	mu       sync.Mutex
	tokens   map[string]streamToken
	prepared map[string]time.Time
	pushRuns map[string]pushRun
}

func newStreamToken() (string, error) {
	var bytes [32]byte
	if _, err := rand.Read(bytes[:]); err != nil {
		return "", err
	}
	return hex.EncodeToString(bytes[:]), nil
}

func validStreamToken(token string) bool {
	if len(token) != 64 {
		return false
	}
	for _, value := range token {
		if !(value >= '0' && value <= '9') && !(value >= 'a' && value <= 'f') {
			return false
		}
	}
	return true
}

func (s *streamTokens) revokeStream(streamID string) {
	s.mu.Lock()
	defer s.mu.Unlock()
	maps.DeleteFunc(s.tokens, func(_ string, token streamToken) bool { return token.streamID == streamID })
}

func (s *streamTokens) expireLocked(now time.Time) {
	for value, token := range s.tokens {
		if now.Before(token.expiresAt) {
			continue
		}
		delete(s.tokens, value)
	}
	maps.DeleteFunc(s.prepared, func(_ string, expiresAt time.Time) bool { return !now.Before(expiresAt) })
}

func (s *streamTokens) expire(now time.Time) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.expireLocked(now)
}

type playSource struct {
	LiveID       string `json:"live_id"`
	SourceID     string `json:"source_id"`
	PushDeviceID string `json:"push_device_id"`
}

type playbackURLs struct {
	StreamID  string    `json:"stream_id"`
	Token     string    `json:"token"`
	ExpiresAt time.Time `json:"expires_at"`
	RTMPURL   string    `json:"rtmp_url"`
	RTSPURL   string    `json:"rtsp_url"`
	FLVURL    string    `json:"flv_url"`
	HLSURL    string    `json:"hls_url"`
	WHEPURL   string    `json:"whep_url"`
}

var errSourceNotRunning = errors.New("source is not running")
var errMediaNotReady = errors.New("media source is not ready")

func (s *infrastructureServer) newPlaybackURLs(source playSource) (playbackURLs, error) {
	s.live.mu.Lock()
	defer s.live.mu.Unlock()
	s.rtspPullMu.Lock()
	defer s.rtspPullMu.Unlock()
	s.tokens.mu.Lock()
	defer s.tokens.mu.Unlock()
	var streamID string
	if source.LiveID != "" {
		for _, session := range s.live.sessions {
			if session.streamID == source.LiveID && session.key.historyID == "" {
				streamID = session.streamID
			}
		}
	} else if source.SourceID != "" {
		if session, ok := s.rtspPulls[source.SourceID]; ok {
			streamID = session.streamID
		}
	} else if source.PushDeviceID != "" {
		if run, ok := s.tokens.pushRuns[source.PushDeviceID]; ok {
			streamID = run.streamID
		}
	}
	if streamID == "" || !s.streamRunningLocked(streamID) {
		return playbackURLs{}, errSourceNotRunning
	}
	if source.LiveID == "" && !s.mediaReady(streamID) {
		return playbackURLs{}, errMediaNotReady
	}
	value, err := newStreamToken()
	if err != nil {
		return playbackURLs{}, err
	}
	expiresAt := s.live.sip.now().Add(s.tokenTTL)
	s.tokens.tokens[value] = streamToken{streamID: streamID, expiresAt: expiresAt}
	server := s.media.server
	httpOrigin := server.publicHTTPOrigin()
	path := "/" + streamID + "/" + value
	result := playbackURLs{
		StreamID: streamID, Token: value, ExpiresAt: expiresAt,
		RTMPURL: (&url.URL{Scheme: "rtmp", Host: net.JoinHostPort(server.mediaIP, strconv.Itoa(int(server.rtmpPort))), Path: path}).String(),
		RTSPURL: (&url.URL{Scheme: "rtsp", Host: net.JoinHostPort(server.mediaIP, strconv.Itoa(int(server.rtspPort))), Path: path}).String(),
		FLVURL:  httpOrigin + path + ".flv",
		HLSURL:  httpOrigin + "/play/hls" + path + "/index.m3u8",
		WHEPURL: httpOrigin + "/play/whep" + path,
	}
	s.logger.Info("play URLs issued", "stream_id", streamID, "token", value, "expires_at", expiresAt,
		"rtmp_url", result.RTMPURL, "rtsp_url", result.RTSPURL, "flv_url", result.FLVURL, "hls_url", result.HLSURL, "whep_url", result.WHEPURL)
	return result, nil
}

func (s *infrastructureServer) handlePlay(writer http.ResponseWriter, request *http.Request) {
	var command struct {
		Source playSource `json:"source"`
	}
	if !decodeJSON(writer, request, &command) {
		writeHTTPError(writer, http.StatusBadRequest, "invalid_request")
		return
	}
	count := 0
	for _, id := range []string{command.Source.LiveID, command.Source.SourceID, command.Source.PushDeviceID} {
		if id != "" {
			if !validUUIDv4(id) {
				writeHTTPError(writer, http.StatusBadRequest, "invalid_request")
				return
			}
			count++
		}
	}
	if count != 1 {
		writeHTTPError(writer, http.StatusBadRequest, "invalid_request")
		return
	}
	result, err := s.newPlaybackURLs(command.Source)
	if errors.Is(err, errMediaNotReady) {
		writeHTTPError(writer, http.StatusConflict, "media_not_ready")
		return
	}
	if errors.Is(err, errSourceNotRunning) {
		writeHTTPError(writer, http.StatusConflict, "not_running")
		return
	}
	if err != nil {
		writeHTTPError(writer, http.StatusInternalServerError, "operation_failed")
		return
	}
	writer.Header().Set("Cache-Control", "no-store")
	writeJSON(writer, http.StatusCreated, result)
}

func (s *infrastructureServer) streamRunningLocked(streamID string) bool {
	for _, session := range s.live.sessions {
		if session.streamID == streamID && session.key.historyID == "" && session.state == liveStreaming {
			_, stopping := s.live.stoppingDevices[session.key.deviceID]
			return !stopping
		}
	}
	for _, session := range s.rtspPulls {
		if session.streamID == streamID && !session.starting {
			return true
		}
	}
	for _, run := range s.tokens.pushRuns {
		if run.streamID == streamID {
			return true
		}
	}
	return false
}

func (s *infrastructureServer) handleVerify(writer http.ResponseWriter, request *http.Request) {
	if secret := s.media.server.controlToken; secret != "" {
		if request.Header.Get("Authorization") != "Bearer "+secret {
			writeHTTPError(writer, http.StatusForbidden, "forbidden")
			return
		}
	} else {
		host, _, err := net.SplitHostPort(request.RemoteAddr)
		if err != nil || !net.ParseIP(host).IsLoopback() {
			writeHTTPError(writer, http.StatusForbidden, "forbidden")
			return
		}
	}
	var command struct {
		Token     string `json:"token"`
		Operation string `json:"operation"`
		StreamID  string `json:"stream_id"`
	}
	if !decodeJSON(writer, request, &command) || !validStreamToken(command.Token) || command.StreamID == "" ||
		(command.Operation != "play" && command.Operation != "publish") {
		writeHTTPError(writer, http.StatusBadRequest, "invalid_request")
		return
	}
	if reason := s.consumeStreamToken(request.Context(), command.Token, command.Operation, command.StreamID); reason != "" {
		s.logger.Info("stream authorization rejected", "reason", reason, "token", command.Token, "operation", command.Operation, "stream_id", command.StreamID)
		writeHTTPError(writer, http.StatusForbidden, "forbidden")
		return
	}
	s.logger.Info("stream authorization consumed", "token", command.Token, "operation", command.Operation, "stream_id", command.StreamID)
	writer.WriteHeader(http.StatusOK)
}

func (s *infrastructureServer) consumeStreamToken(ctx context.Context, value, operation, streamID string) string {
	s.live.mu.Lock()
	s.rtspPullMu.Lock()
	s.tokens.mu.Lock()
	defer s.live.mu.Unlock()
	defer s.rtspPullMu.Unlock()
	defer s.tokens.mu.Unlock()
	now := s.live.sip.now()
	if operation == "publish" {
		if value != streamID {
			return "publish_identity_mismatch"
		}
		deviceID, err := s.sources.consumePushCredential(ctx, value, now)
		if err != nil {
			s.logger.Error("push credential consume failed", "error", err)
			return "credential_store_failed"
		}
		if deviceID == "" {
			return "push_credential_unavailable"
		}
		s.tokens.pushRuns[deviceID] = pushRun{streamID: value, verifiedAt: now}
		return ""
	}
	token, ok := s.tokens.tokens[value]
	var reason string
	switch {
	case !ok:
		reason = "token_not_found"
	case !now.Before(token.expiresAt):
		reason = "token_expired"
	case token.streamID != streamID:
		reason = "stream_id_mismatch"
	}
	if reason != "" {
		return reason
	}
	for _, session := range s.live.sessions {
		if session.key.historyID != streamID {
			continue
		}
		_, stopping := s.live.stoppingDevices[session.key.deviceID]
		if stopping || session.state != livePreparing || session.ctx.Err() != nil {
			return "playback_not_prepared"
		}
		delete(s.tokens.tokens, value)
		close(session.claimed)
		return ""
	}
	if !s.streamRunningLocked(streamID) {
		return "source_not_running"
	}
	delete(s.tokens.tokens, value)
	return ""
}
