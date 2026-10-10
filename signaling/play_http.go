package main

import (
	"errors"
	"net/http"
)

func (s *infrastructureServer) handleChannelPlay(writer http.ResponseWriter, request *http.Request) {
	deviceID, channelID := request.PathValue("device_id"), request.PathValue("channel_id")
	if !validDigits(deviceID, 20) || !validDigits(channelID, 20) {
		writeHTTPError(writer, http.StatusBadRequest, "invalid_request")
		return
	}
	view, err := s.live.startLive(request.Context(), deviceID, channelID)
	if err != nil {
		switch {
		case errors.Is(err, errDeviceNotFound):
			writeHTTPError(writer, http.StatusNotFound, "device_not_found")
		case errors.Is(err, errLiveStopping), errors.Is(err, errLiveChanged):
			writeHTTPError(writer, http.StatusConflict, "live_stopping")
		case errors.Is(err, errDeviceOffline):
			writeHTTPError(writer, http.StatusConflict, "device_offline")
		case errors.Is(err, errDeviceStopping):
			writeHTTPError(writer, http.StatusConflict, "device_stopping")
		case errors.Is(err, errChannelNotFound):
			writeHTTPError(writer, http.StatusNotFound, "channel_not_found")
		case errors.Is(err, errChannelOffline):
			writeHTTPError(writer, http.StatusConflict, "channel_offline")
		default:
			s.logger.Error("live start failed", "device_id", deviceID, "channel_id", channelID, "error", err)
			writeHTTPError(writer, http.StatusBadGateway, "live_start_failed")
		}
		return
	}
	result, err := s.newPlaybackURLs(playSource{LiveID: view.streamID})
	if err != nil {
		writeHTTPError(writer, http.StatusConflict, "live_stopping")
		return
	}
	writer.Header().Set("Cache-Control", "no-store")
	writeJSON(writer, http.StatusCreated, result)
}

func (s *infrastructureServer) handleLiveDelete(writer http.ResponseWriter, request *http.Request) {
	err := s.live.stopLiveID(request.Context(), request.PathValue("live_id"))
	if errors.Is(err, errLiveNotFound) {
		writeHTTPError(writer, http.StatusNotFound, "live_not_found")
		return
	}
	if err != nil {
		s.logger.Error("live stop failed", "live_id", request.PathValue("live_id"), "error", err)
		writeHTTPError(writer, http.StatusBadGateway, "live_stop_failed")
		return
	}
	writer.WriteHeader(http.StatusNoContent)
}
