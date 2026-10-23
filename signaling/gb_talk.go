package main

import (
	"encoding/json"
	"errors"
	"net/http"
	"strings"
)

type talkTicket struct {
	TalkID    string `json:"talk_id"`
	UploadURL string `json:"upload_url"`
}

func (s *infrastructureServer) handleTalkCreate(writer http.ResponseWriter, request *http.Request) {
	deviceID := request.PathValue("device_id")
	var command struct{}
	if !validDigits(deviceID, 20) || !decodeJSON(writer, request, &command) {
		writeHTTPError(writer, http.StatusBadRequest, "invalid_request")
		return
	}
	result, err := s.live.startBroadcast(request.Context(), deviceID)
	if err != nil {
		switch {
		case errors.Is(err, errDeviceNotFound):
			writeHTTPError(writer, http.StatusNotFound, "device_not_found")
		case errors.Is(err, errDeviceOffline):
			writeHTTPError(writer, http.StatusConflict, "device_offline")
		case errors.Is(err, errDeviceStopping):
			writeHTTPError(writer, http.StatusConflict, "device_stopping")
		case errors.Is(err, errBroadcastBusy):
			writeHTTPError(writer, http.StatusConflict, "talk_busy")
		default:
			s.logger.Warn("talk startup failed", "device_id", deviceID, "error", err)
			writeHTTPError(writer, http.StatusBadGateway, "talk_start_failed")
		}
		return
	}
	origin := "ws" + strings.TrimPrefix(s.media.server.publicHTTPOrigin(), "http")
	upload := origin + "/publish/gb28181/audio/" + result.mediaID + "/" + result.uploadToken
	writer.Header().Set("Cache-Control", "no-store")
	writer.Header().Set("Content-Type", "application/json")
	writer.WriteHeader(http.StatusCreated)
	if err := json.NewEncoder(writer).Encode(talkTicket{TalkID: result.mediaID, UploadURL: upload}); err != nil || request.Context().Err() != nil {
		cleanup, cancel := s.media.timeoutContext()
		defer cancel()
		_ = s.live.stopBroadcast(cleanup, deviceID, result.mediaID)
	}
}

func (s *infrastructureServer) handleTalkGet(writer http.ResponseWriter, request *http.Request) {
	deviceID := request.PathValue("device_id")
	if !validDigits(deviceID, 20) {
		writeHTTPError(writer, http.StatusBadRequest, "invalid_request")
		return
	}
	s.live.mu.Lock()
	session := s.live.broadcasts[deviceID]
	if session == nil || session.ctx.Err() != nil {
		s.live.mu.Unlock()
		writeHTTPError(writer, http.StatusNotFound, "talk_not_found")
		return
	}
	id := session.mediaID
	s.live.mu.Unlock()
	writer.Header().Set("Cache-Control", "no-store")
	writeJSON(writer, http.StatusOK, map[string]string{"talk_id": id, "device_id": deviceID})
}

func (s *infrastructureServer) handleTalkDelete(writer http.ResponseWriter, request *http.Request) {
	deviceID := request.PathValue("device_id")
	var command struct {
		TalkID string `json:"talk_id"`
	}
	if !validDigits(deviceID, 20) || !decodeJSON(writer, request, &command) || !validStreamToken(command.TalkID) {
		writeHTTPError(writer, http.StatusBadRequest, "invalid_request")
		return
	}
	err := s.live.stopBroadcast(request.Context(), deviceID, command.TalkID)
	if errors.Is(err, errBroadcastNotFound) {
		writeHTTPError(writer, http.StatusNotFound, "talk_not_found")
		return
	}
	if err != nil {
		s.logger.Warn("talk cleanup failed", "device_id", deviceID, "talk_id", command.TalkID, "error", err)
		writeHTTPError(writer, http.StatusBadGateway, "talk_stop_failed")
		return
	}
	writer.WriteHeader(http.StatusNoContent)
}
