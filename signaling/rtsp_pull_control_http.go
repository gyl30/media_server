package main

import (
	"context"
	"encoding/json"
	"errors"
	"net/http"
	"sync"
)

type rtspPullControlCreateRequest struct {
	StreamName string          `json:"stream_name"`
	URL        string          `json:"url"`
	Username   json.RawMessage `json:"username"`
	Password   json.RawMessage `json:"password"`
}

func (s *infrastructureServer) handleRTSPPullCreate(writer http.ResponseWriter, request *http.Request) {
	var payload rtspPullControlCreateRequest
	if !decodeJSON(writer, request, &payload) || payload.StreamName == "" || payload.URL == "" {
		writeHTTPError(writer, http.StatusBadRequest, "invalid_request")
		return
	}
	username, usernameValid := decodeOptionalString(payload.Username)
	password, passwordValid := decodeOptionalString(payload.Password)
	if !usernameValid || !passwordValid || (password != nil && (username == nil || *username == "")) {
		writeHTTPError(writer, http.StatusBadRequest, "invalid_request")
		return
	}
	command := rtspPullCreateRequest{
		StreamName: payload.StreamName, URL: payload.URL, Username: username, Password: password,
	}

	server, ok := s.rtspPullServer(command.StreamName)
	if !ok {
		server, ok = s.registry.selectOnline()
	}
	if !ok {
		writeHTTPError(writer, http.StatusServiceUnavailable, "no_media_server")
		return
	}

	if err := s.media.createRTSPPull(request.Context(), server, command); err != nil {
		s.writeRTSPPullError(writer, "create", command.StreamName, err)
		return
	}
	s.rtspPullMu.Lock()
	s.rtspPulls[command.StreamName] = server
	s.rtspPullMu.Unlock()
	if !s.registry.isOnline(server) {
		if s.removeRTSPPull(command.StreamName, server) {
			cleanupContext, cancel := context.WithTimeout(context.Background(), s.cfg.mediaRequestTimeout)
			if err := s.media.deleteRTSPPull(cleanupContext, server, command.StreamName); err != nil {
				s.logger.Warn("rtsp pull cleanup failed", "stream_name", command.StreamName, "server_id", server.serverID, "error", err)
			}
			cancel()
		}
		writeHTTPError(writer, http.StatusServiceUnavailable, "no_media_server")
		return
	}
	writeJSON(writer, http.StatusCreated, map[string]string{"result": "ok"})
}

func decodeOptionalString(raw json.RawMessage) (*string, bool) {
	if raw == nil {
		return nil, true
	}
	var value *string
	if err := json.Unmarshal(raw, &value); err != nil {
		return nil, false
	}
	return value, value != nil
}

func (s *infrastructureServer) handleRTSPPullDelete(writer http.ResponseWriter, request *http.Request) {
	var command struct {
		StreamName string `json:"stream_name"`
	}
	if !decodeJSON(writer, request, &command) || command.StreamName == "" {
		writeHTTPError(writer, http.StatusBadRequest, "invalid_request")
		return
	}

	server, ok := s.takeRTSPPullServer(command.StreamName)
	if !ok {
		writeHTTPError(writer, http.StatusNotFound, "not_found")
		return
	}
	if err := s.media.deleteRTSPPull(request.Context(), server, command.StreamName); err != nil {
		var rejection *mediaServerHTTPRejection
		if !errors.As(err, &rejection) || rejection.status != http.StatusNotFound {
			s.restoreRTSPPull(command.StreamName, server)
		}
		s.writeRTSPPullError(writer, "delete", command.StreamName, err)
		return
	}
	writeJSON(writer, http.StatusOK, map[string]string{"result": "ok"})
}

func (s *infrastructureServer) rtspPullServer(streamName string) (mediaServerInstance, bool) {
	s.rtspPullMu.Lock()
	server, ok := s.rtspPulls[streamName]
	s.rtspPullMu.Unlock()
	if !ok || !s.registry.isOnline(server) {
		return mediaServerInstance{}, false
	}
	return server, true
}

func (s *infrastructureServer) takeRTSPPullServer(streamName string) (mediaServerInstance, bool) {
	s.rtspPullMu.Lock()
	server, ok := s.rtspPulls[streamName]
	if ok {
		delete(s.rtspPulls, streamName)
	}
	s.rtspPullMu.Unlock()
	if !ok || !s.registry.isOnline(server) {
		return mediaServerInstance{}, false
	}
	return server, true
}

func (s *infrastructureServer) restoreRTSPPull(streamName string, server mediaServerInstance) {
	s.rtspPullMu.Lock()
	if _, exists := s.rtspPulls[streamName]; !exists {
		s.rtspPulls[streamName] = server
	}
	s.rtspPullMu.Unlock()
}

func (s *infrastructureServer) removeRTSPPull(streamName string, expected mediaServerInstance) bool {
	s.rtspPullMu.Lock()
	defer s.rtspPullMu.Unlock()
	server, ok := s.rtspPulls[streamName]
	if !ok || server.serverID != expected.serverID || server.instanceID != expected.instanceID {
		return false
	}
	delete(s.rtspPulls, streamName)
	return true
}

func (s *infrastructureServer) removeRTSPPullsForMediaServer(expected mediaServerInstance) {
	s.rtspPullMu.Lock()
	for streamName, server := range s.rtspPulls {
		if server.serverID == expected.serverID && server.instanceID == expected.instanceID {
			delete(s.rtspPulls, streamName)
		}
	}
	s.rtspPullMu.Unlock()
}

func (s *infrastructureServer) shutdownRTSPPulls(ctx context.Context) {
	s.rtspPullMu.Lock()
	pulls := s.rtspPulls
	s.rtspPulls = make(map[string]mediaServerInstance)
	s.rtspPullMu.Unlock()

	var wait sync.WaitGroup
	for streamName, server := range pulls {
		wait.Add(1)
		go func() {
			defer wait.Done()
			if err := s.media.deleteRTSPPull(ctx, server, streamName); err != nil {
				s.logger.Warn("rtsp pull shutdown failed", "stream_name", streamName, "server_id", server.serverID, "error", err)
			}
		}()
	}
	wait.Wait()
}

func (s *infrastructureServer) writeRTSPPullError(writer http.ResponseWriter, operation, streamName string, err error) {
	var rejection *mediaServerHTTPRejection
	if errors.As(err, &rejection) {
		switch {
		case rejection.status == http.StatusBadRequest && rejection.code == "invalid_request":
			writeHTTPError(writer, http.StatusBadRequest, "invalid_request")
			return
		case rejection.status == http.StatusConflict && rejection.code == "conflict":
			writeHTTPError(writer, http.StatusConflict, "conflict")
			return
		case rejection.status == http.StatusNotFound && rejection.code == "not_found":
			writeHTTPError(writer, http.StatusNotFound, "not_found")
			return
		}
	}
	s.logger.Error("rtsp pull control failed", "operation", operation, "stream_name", streamName, "error", err)
	writeHTTPError(writer, http.StatusBadGateway, "rtsp_pull_"+operation+"_failed")
}
