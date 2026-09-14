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
	view, ok := s.startLive(writer, request, command.DeviceID, command.ChannelID)
	if !ok {
		return
	}
	writeJSON(writer, http.StatusCreated, map[string]any{
		"result":      "ok",
		"stream_id":   view.streamID,
		"stream_name": view.streamName,
		"state":       view.state,
		"ssrc":        view.ssrc,
		"rtp_port":    view.rtpPort,
	})
}

func (s *infrastructureServer) startLive(writer http.ResponseWriter, request *http.Request, deviceID, channelID string) (liveView, bool) {
	view, err := s.live.startLive(request.Context(), deviceID, channelID)
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
			s.logger.Error("live start failed", "device_id", deviceID, "channel_id", channelID, "error", err)
			writeHTTPError(writer, http.StatusBadGateway, "live_start_failed")
		}
		return liveView{}, false
	}
	return view, true
}

func (s *infrastructureServer) handleLiveStop(writer http.ResponseWriter, request *http.Request) {
	var command liveControlRequest
	if !decodeJSON(writer, request, &command) || !validDigits(command.DeviceID, 20) || !validDigits(command.ChannelID, 20) {
		writeHTTPError(writer, http.StatusBadRequest, "invalid_request")
		return
	}
	s.stopLive(writer, request, command.DeviceID, command.ChannelID, "")
}

func (s *infrastructureServer) stopLive(writer http.ResponseWriter, request *http.Request, deviceID, channelID, streamID string) {
	live, mediaStopped, err := s.live.stopLiveRuntime(request.Context(), deviceID, channelID, streamID)
	if mediaStopped {
		_, stateErr := s.runtimes.acknowledgeSourceStopped(
			live.server, live.streamID, live.streamName, "", "gb28181")
		err = errors.Join(err, stateErr)
	}
	if err != nil {
		switch {
		case errors.Is(err, errLiveNotFound):
			writeHTTPError(writer, http.StatusNotFound, "live_not_found")
		case errors.Is(err, errLiveChanged):
			writeHTTPError(writer, http.StatusConflict, "live_changed")
		default:
			s.logger.Error("live stop failed", "device_id", deviceID, "channel_id", channelID, "error", err)
			writeHTTPError(writer, http.StatusBadGateway, "live_stop_failed")
		}
		return
	}

	writeJSON(writer, http.StatusOK, map[string]string{"result": "ok"})
}
