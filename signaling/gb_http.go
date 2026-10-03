package main

import (
	"errors"
	"net/http"
	"time"
)

type deviceResponse struct {
	DeviceID string    `json:"device_id"`
	Name     string    `json:"name"`
	Online   bool      `json:"online"`
	LastSeen time.Time `json:"last_seen,omitzero"`
}

type channelResponse struct {
	DeviceID  string        `json:"device_id"`
	ChannelID string        `json:"channel_id"`
	Name      string        `json:"name"`
	ParentID  string        `json:"parent_id"`
	Status    string        `json:"status"`
	Live      *liveResponse `json:"live,omitempty"`
}

type liveResponse struct {
	StreamID   string    `json:"stream_id"`
	StreamName string    `json:"stream_name"`
	State      liveState `json:"state"`
}

type liveStopRequest struct {
	StreamID string `json:"stream_id"`
}

func (s *infrastructureServer) handleDeviceList(writer http.ResponseWriter, request *http.Request) {
	devices, err := s.live.sip.deviceStore.list(request.Context())
	if err != nil {
		s.writeDeviceError(writer, err)
		return
	}
	response := make([]deviceResponse, 0, len(devices))
	for _, device := range devices {
		response = append(response, s.deviceResponse(device))
	}
	writeJSON(writer, http.StatusOK, map[string]any{"devices": response})
}

func (s *infrastructureServer) deviceResponse(device gbDevice) deviceResponse {
	response := deviceResponse{DeviceID: device.deviceID, Name: device.name}
	if registration, ok := s.live.sip.devices.get(device.deviceID); ok {
		response.Online = registration.online && s.live.sip.now().Before(registration.expiresAt)
		response.LastSeen = registration.lastHeartbeat
	}
	return response
}

func (s *infrastructureServer) handleDeviceCreate(writer http.ResponseWriter, request *http.Request) {
	var command struct {
		DeviceID string `json:"device_id"`
		Name     string `json:"name"`
	}
	if !decodeJSON(writer, request, &command) {
		writeHTTPError(writer, http.StatusBadRequest, "invalid_request")
		return
	}
	device := gbDevice{deviceID: command.DeviceID, name: command.Name}
	if err := s.live.sip.deviceStore.create(request.Context(), device); err != nil {
		s.writeDeviceError(writer, err)
		return
	}
	writeJSON(writer, http.StatusCreated, s.deviceResponse(device))
}

func (s *infrastructureServer) handleDeviceGet(writer http.ResponseWriter, request *http.Request) {
	device, err := s.live.sip.deviceStore.get(request.Context(), request.PathValue("device_id"))
	if err != nil {
		s.writeDeviceError(writer, err)
		return
	}
	writeJSON(writer, http.StatusOK, s.deviceResponse(device))
}

func (s *infrastructureServer) writeDeviceError(writer http.ResponseWriter, err error) {
	switch {
	case errors.Is(err, errDeviceNotFound):
		writeHTTPError(writer, http.StatusNotFound, "device_not_found")
	case errors.Is(err, errDeviceExists):
		writeHTTPError(writer, http.StatusConflict, "device_exists")
	case errors.Is(err, errInvalidDevice):
		writeHTTPError(writer, http.StatusBadRequest, "invalid_request")
	default:
		s.logger.Error("device store failed", "error", err)
		writeHTTPError(writer, http.StatusInternalServerError, "device_store_failed")
	}
}

func (s *infrastructureServer) handleChannelList(writer http.ResponseWriter, request *http.Request) {
	deviceID := request.PathValue("device_id")
	if !validDigits(deviceID, 20) {
		writeHTTPError(writer, http.StatusBadRequest, "invalid_request")
		return
	}
	if _, err := s.live.sip.deviceStore.get(request.Context(), deviceID); err != nil {
		s.writeDeviceError(writer, err)
		return
	}
	channels := s.live.sip.channels.list(deviceID)
	response := make([]channelResponse, 0, len(channels))
	for _, channel := range channels {
		item := channelResponse{
			DeviceID: channel.deviceID, ChannelID: channel.id, Name: channel.name,
			ParentID: channel.parentID, Status: channel.status,
		}
		if live, ok := s.live.live(channel.deviceID, channel.id); ok {
			item.Live = &liveResponse{StreamID: live.streamID, StreamName: live.streamName, State: live.state}
		}
		response = append(response, item)
	}
	writeJSON(writer, http.StatusOK, map[string]any{"channels": response})
}

func (s *infrastructureServer) handleChannelLiveStart(writer http.ResponseWriter, request *http.Request) {
	deviceID := request.PathValue("device_id")
	channelID := request.PathValue("channel_id")
	if !validDigits(deviceID, 20) || !validDigits(channelID, 20) {
		writeHTTPError(writer, http.StatusBadRequest, "invalid_request")
		return
	}
	view, ok := s.startLive(writer, request, deviceID, channelID)
	if !ok {
		return
	}
	writeJSON(writer, http.StatusCreated, map[string]any{
		"stream_id":   view.streamID,
		"stream_name": view.streamName,
		"state":       view.state,
	})
}

func (s *infrastructureServer) handleChannelLiveStop(writer http.ResponseWriter, request *http.Request) {
	deviceID := request.PathValue("device_id")
	channelID := request.PathValue("channel_id")
	var command liveStopRequest
	if !validDigits(deviceID, 20) || !validDigits(channelID, 20) ||
		!decodeJSON(writer, request, &command) || !validUUIDv4(command.StreamID) {
		writeHTTPError(writer, http.StatusBadRequest, "invalid_request")
		return
	}
	if s.stopLive(writer, request, deviceID, channelID, command.StreamID) {
		writer.WriteHeader(http.StatusNoContent)
	}
}
