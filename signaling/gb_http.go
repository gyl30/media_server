package main

import (
	"context"
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
	LiveID     string    `json:"live_id"`
	State      liveState `json:"state"`
	MediaReady bool      `json:"media_ready"`
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
	case errors.Is(err, errDeviceStopping):
		writeHTTPError(writer, http.StatusConflict, "device_stopping")
	default:
		s.logger.Error("device store failed", "error", err)
		writeHTTPError(writer, http.StatusInternalServerError, "device_store_failed")
	}
}

func (s *infrastructureServer) handleDeviceDelete(writer http.ResponseWriter, request *http.Request) {
	err := s.live.deleteDevice(request.Context(), request.PathValue("device_id"))
	if errors.Is(err, errDeviceNotFound) || errors.Is(err, errDeviceStopping) {
		s.writeDeviceError(writer, err)
		return
	}
	if err != nil {
		s.logger.Error("device delete failed", "device_id", request.PathValue("device_id"), "error", err)
		writeHTTPError(writer, http.StatusBadGateway, "device_delete_failed")
		return
	}
	writer.WriteHeader(http.StatusNoContent)
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
			item.Live = &liveResponse{LiveID: live.streamID, State: live.state, MediaReady: live.state == liveStreaming && s.mediaReady(live.streamID)}
		}
		response = append(response, item)
	}
	writeJSON(writer, http.StatusOK, map[string]any{"channels": response})
}

func (s *infrastructureServer) handleCatalogQuery(writer http.ResponseWriter, request *http.Request) {
	deviceID := request.PathValue("device_id")
	if !validDigits(deviceID, 20) {
		writeHTTPError(writer, http.StatusBadRequest, "invalid_request")
		return
	}
	if _, err := s.live.sip.deviceStore.get(request.Context(), deviceID); err != nil {
		s.writeDeviceError(writer, err)
		return
	}
	s.live.mu.Lock()
	if _, stopping := s.live.stoppingDevices[deviceID]; stopping {
		s.live.mu.Unlock()
		writeHTTPError(writer, http.StatusConflict, "device_stopping")
		return
	}
	device, online := s.live.sip.devices.getOnline(deviceID, s.live.sip.now())
	if !online {
		s.live.mu.Unlock()
		writeHTTPError(writer, http.StatusConflict, "device_offline")
		return
	}
	sn := int(s.live.sip.catalogSN.Add(1))
	s.live.sip.channels.beginQuery(deviceID, sn)
	s.live.mu.Unlock()
	ctx, cancel := context.WithTimeout(request.Context(), 5*time.Second)
	defer cancel()
	if err := s.live.sip.sendCatalogQuery(ctx, device, sn); err != nil {
		s.live.sip.channels.cancelQuery(deviceID, sn)
		if errors.Is(ctx.Err(), context.DeadlineExceeded) {
			writeHTTPError(writer, http.StatusGatewayTimeout, "catalog_query_timeout")
			return
		}
		s.logger.Warn("Catalog query failed", "device_id", deviceID, "error", err)
		writeHTTPError(writer, http.StatusBadGateway, "catalog_query_failed")
		return
	}
	// SIP 受理不代表异步目录响应已经收齐，现有通道查询返回实际收到的目录。
	writeJSON(writer, http.StatusAccepted, map[string]any{"device_id": deviceID, "sn": sn, "status": "accepted"})
}
