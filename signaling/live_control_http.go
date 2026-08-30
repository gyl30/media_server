package main

import (
	"errors"
	"net/http"
)

type liveControlRequest struct {
	DeviceID  string `json:"device_id"`
	ChannelID string `json:"channel_id"`
}

func (s *infrastructureServer) handleLiveStart(writer http.ResponseWriter, request *http.Request) {
	var command liveControlRequest
	if !decodeJSON(writer, request, &command) || !validDigits(command.DeviceID, 20) || !validDigits(command.ChannelID, 20) {
		writeHTTPError(writer, http.StatusBadRequest, "invalid_request")
		return
	}

	view, err := s.live.startLive(request.Context(), command.DeviceID, command.ChannelID)
	if err != nil {
		switch {
		case errors.Is(err, errLiveExists):
			writeHTTPError(writer, http.StatusConflict, "live_exists")
		case errors.Is(err, errDeviceOffline):
			writeHTTPError(writer, http.StatusConflict, "device_offline")
		case errors.Is(err, errChannelUnavailable):
			writeHTTPError(writer, http.StatusConflict, "channel_unavailable")
		case errors.Is(err, errNoMediaServer):
			writeHTTPError(writer, http.StatusServiceUnavailable, "no_media_server")
		default:
			s.logger.Error("live start failed", "device_id", command.DeviceID, "channel_id", command.ChannelID, "error", err)
			writeHTTPError(writer, http.StatusBadGateway, "live_start_failed")
		}
		return
	}

	writeJSON(writer, http.StatusCreated, map[string]any{
		"result":      "ok",
		"stream_name": view.streamName,
		"state":       view.state,
		"ssrc":        view.ssrc,
		"rtp_port":    view.rtpPort,
	})
}

func (s *infrastructureServer) handleLiveStop(writer http.ResponseWriter, request *http.Request) {
	var command liveControlRequest
	if !decodeJSON(writer, request, &command) || !validDigits(command.DeviceID, 20) || !validDigits(command.ChannelID, 20) {
		writeHTTPError(writer, http.StatusBadRequest, "invalid_request")
		return
	}

	if err := s.live.stopLive(request.Context(), command.DeviceID, command.ChannelID); err != nil {
		if errors.Is(err, errLiveNotFound) {
			writeHTTPError(writer, http.StatusNotFound, "live_not_found")
			return
		}
		s.logger.Error("live stop failed", "device_id", command.DeviceID, "channel_id", command.ChannelID, "error", err)
		writeHTTPError(writer, http.StatusBadGateway, "live_stop_failed")
		return
	}

	writeJSON(writer, http.StatusOK, map[string]string{"result": "ok"})
}
