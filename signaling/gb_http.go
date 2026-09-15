package main

import (
	"net/http"
	"time"
)

type deviceResponse struct {
	DeviceID string    `json:"device_id"`
	Online   bool      `json:"online"`
	LastSeen time.Time `json:"last_seen"`
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

func (s *infrastructureServer) handleDeviceList(writer http.ResponseWriter, _ *http.Request) {
	now := s.live.sip.now()
	devices := s.live.sip.devices.snapshot(now)
	response := make([]deviceResponse, 0, len(devices))
	for _, device := range devices {
		response = append(response, deviceResponse{
			DeviceID: device.id, Online: device.online, LastSeen: device.lastHeartbeat,
		})
	}
	writeJSON(writer, http.StatusOK, map[string]any{"devices": response})
}

func (s *infrastructureServer) handleChannelList(writer http.ResponseWriter, request *http.Request) {
	deviceID := request.PathValue("device_id")
	if !validDigits(deviceID, 20) {
		writeHTTPError(writer, http.StatusBadRequest, "invalid_request")
		return
	}
	if _, ok := s.live.sip.devices.get(deviceID); !ok {
		writeHTTPError(writer, http.StatusNotFound, "device_not_found")
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
