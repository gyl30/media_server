package main

import (
	"context"
	"errors"
	"maps"
	"net"
	"net/http"
	"net/url"
	"strconv"
	"time"

	"github.com/google/uuid"
)

type pushDeviceResponse struct {
	DeviceID   string    `json:"device_id"`
	Name       string    `json:"name"`
	State      string    `json:"state"`
	Token      string    `json:"token,omitempty"`
	ExpiresAt  time.Time `json:"expires_at,omitzero"`
	VerifiedAt time.Time `json:"verified_at,omitzero"`
}

func (s *infrastructureServer) pushDeviceResponseLocked(device pushDevice) pushDeviceResponse {
	response := pushDeviceResponse{DeviceID: device.deviceID, Name: device.name, State: "idle"}
	if run, ok := s.tokens.pushRuns[device.deviceID]; ok {
		response.Token = run.streamID
		if run.verifiedAt.IsZero() {
			response.State = "issued"
			response.ExpiresAt = s.tokens.tokens[run.streamID].expiresAt
		} else {
			response.State = "verified"
			response.VerifiedAt = run.verifiedAt
		}
	}
	return response
}

func (s *infrastructureServer) handlePushDeviceList(writer http.ResponseWriter, request *http.Request) {
	devices, err := s.sources.listPushDevices(request.Context())
	if err != nil {
		s.writeDeviceError(writer, err)
		return
	}
	s.tokens.mu.Lock()
	s.tokens.expireLocked(s.live.sip.now())
	response := make([]pushDeviceResponse, 0, len(devices))
	for _, device := range devices {
		response = append(response, s.pushDeviceResponseLocked(device))
	}
	s.tokens.mu.Unlock()
	writeJSON(writer, http.StatusOK, map[string]any{"devices": response})
}

func (s *infrastructureServer) handlePushDeviceCreate(writer http.ResponseWriter, request *http.Request) {
	var command struct {
		Name string `json:"name"`
	}
	if !decodeJSON(writer, request, &command) {
		writeHTTPError(writer, http.StatusBadRequest, "invalid_request")
		return
	}
	device := pushDevice{deviceID: uuid.NewString(), name: command.Name}
	if err := s.sources.createPushDevice(request.Context(), device); err != nil {
		s.writeDeviceError(writer, err)
		return
	}
	writeJSON(writer, http.StatusCreated, pushDeviceResponse{DeviceID: device.deviceID, Name: device.name, State: "idle"})
}

func (s *infrastructureServer) handlePushDeviceGet(writer http.ResponseWriter, request *http.Request) {
	deviceID := request.PathValue("id")
	if !validUUIDv4(deviceID) {
		writeHTTPError(writer, http.StatusBadRequest, "invalid_request")
		return
	}
	device, err := s.sources.getPushDevice(request.Context(), deviceID)
	if err != nil {
		s.writeDeviceError(writer, err)
		return
	}
	s.tokens.mu.Lock()
	s.tokens.expireLocked(s.live.sip.now())
	response := s.pushDeviceResponseLocked(device)
	s.tokens.mu.Unlock()
	writeJSON(writer, http.StatusOK, response)
}

func (s *infrastructureServer) handlePushDevicePatch(writer http.ResponseWriter, request *http.Request) {
	deviceID := request.PathValue("id")
	var command struct {
		Name string `json:"name"`
	}
	if !validUUIDv4(deviceID) || !decodeJSON(writer, request, &command) {
		writeHTTPError(writer, http.StatusBadRequest, "invalid_request")
		return
	}
	if err := s.sources.patchPushDevice(request.Context(), deviceID, command.Name); err != nil {
		s.writeDeviceError(writer, err)
		return
	}
	s.tokens.mu.Lock()
	s.tokens.expireLocked(s.live.sip.now())
	response := s.pushDeviceResponseLocked(pushDevice{deviceID: deviceID, name: command.Name})
	s.tokens.mu.Unlock()
	writeJSON(writer, http.StatusOK, response)
}

func (s *infrastructureServer) handlePushPublish(writer http.ResponseWriter, request *http.Request) {
	deviceID := request.PathValue("id")
	if !validUUIDv4(deviceID) {
		writeHTTPError(writer, http.StatusBadRequest, "invalid_request")
		return
	}
	s.sourceOperationMu.Lock()
	if _, err := s.sources.getPushDevice(request.Context(), deviceID); err != nil {
		s.sourceOperationMu.Unlock()
		s.writeDeviceError(writer, err)
		return
	}
	s.tokens.mu.Lock()
	now := s.live.sip.now()
	s.tokens.expireLocked(now)
	if _, exists := s.tokens.pushRuns[deviceID]; exists {
		s.tokens.mu.Unlock()
		s.sourceOperationMu.Unlock()
		writeHTTPError(writer, http.StatusConflict, "conflict")
		return
	}
	value, err := newStreamToken()
	if err != nil {
		s.tokens.mu.Unlock()
		s.sourceOperationMu.Unlock()
		writeHTTPError(writer, http.StatusInternalServerError, "operation_failed")
		return
	}
	expiresAt := now.Add(s.tokenTTL)
	s.tokens.tokens[value] = streamToken{operation: "publish", streamID: value, expiresAt: expiresAt}
	s.tokens.pushRuns[deviceID] = pushRun{streamID: value}
	s.tokens.mu.Unlock()
	s.sourceOperationMu.Unlock()
	server := s.media.server
	result := struct {
		Token     string    `json:"token"`
		ExpiresAt time.Time `json:"expires_at"`
		RTMPURL   string    `json:"rtmp_url"`
		RTSPURL   string    `json:"rtsp_url"`
		WHIPURL   string    `json:"whip_url"`
	}{
		Token: value, ExpiresAt: expiresAt,
		RTMPURL: (&url.URL{Scheme: "rtmp", Host: net.JoinHostPort(server.mediaIP, strconv.Itoa(int(server.rtmpPort))), Path: "/live/" + value}).String(),
		RTSPURL: (&url.URL{Scheme: "rtsp", Host: net.JoinHostPort(server.mediaIP, strconv.Itoa(int(server.rtspPort))), Path: "/" + value}).String(),
		WHIPURL: (&url.URL{Scheme: "http", Host: net.JoinHostPort(server.mediaIP, strconv.Itoa(int(server.httpPort))), Path: "/publish/whip/" + value}).String(),
	}
	s.logger.Info("push URLs issued", "device_id", deviceID, "token", value, "expires_at", expiresAt,
		"rtmp_url", result.RTMPURL, "rtsp_url", result.RTSPURL, "whip_url", result.WHIPURL)
	writer.Header().Set("Cache-Control", "no-store")
	writeJSON(writer, http.StatusCreated, result)
}

func (s *infrastructureServer) stopPushDevice(ctx context.Context, deviceID string, remove bool) error {
	s.sourceOperationMu.Lock()
	defer s.sourceOperationMu.Unlock()
	s.tokens.mu.Lock()
	if _, err := s.sources.getPushDevice(ctx, deviceID); err != nil {
		s.tokens.mu.Unlock()
		return err
	}
	if remove {
		if _, err := s.sources.db.ExecContext(ctx, `DELETE FROM push_devices WHERE device_id = ?`, deviceID); err != nil {
			s.tokens.mu.Unlock()
			return err
		}
	}
	run, exists := s.tokens.pushRuns[deviceID]
	delete(s.tokens.pushRuns, deviceID)
	if exists {
		maps.DeleteFunc(s.tokens.tokens, func(_ string, token streamToken) bool { return token.streamID == run.streamID })
	}
	s.tokens.mu.Unlock()
	if exists {
		s.logger.Info("push run stopping", "device_id", deviceID, "stream_id", run.streamID)
		if err := s.media.deleteReceiver(ctx, run.streamID); err != nil && !isMediaServerNotFound(err) {
			return err
		}
	}
	return nil
}

func (s *infrastructureServer) handlePushStop(writer http.ResponseWriter, request *http.Request) {
	deviceID := request.PathValue("id")
	if !validUUIDv4(deviceID) {
		writeHTTPError(writer, http.StatusBadRequest, "invalid_request")
		return
	}
	if err := s.stopPushDevice(request.Context(), deviceID, false); err != nil {
		if errors.Is(err, errDeviceNotFound) {
			s.writeDeviceError(writer, err)
		} else {
			writeHTTPError(writer, http.StatusBadGateway, "push_stop_failed")
		}
		return
	}
	writer.WriteHeader(http.StatusNoContent)
}

func (s *infrastructureServer) handlePushDeviceDelete(writer http.ResponseWriter, request *http.Request) {
	deviceID := request.PathValue("id")
	if !validUUIDv4(deviceID) {
		writeHTTPError(writer, http.StatusBadRequest, "invalid_request")
		return
	}
	if err := s.stopPushDevice(request.Context(), deviceID, true); err != nil {
		if errors.Is(err, errDeviceNotFound) {
			s.writeDeviceError(writer, err)
		} else {
			writeHTTPError(writer, http.StatusBadGateway, "push_delete_failed")
		}
		return
	}
	writer.WriteHeader(http.StatusNoContent)
}

func (s *infrastructureServer) shutdownPushRuns(ctx context.Context) {
	s.tokens.mu.Lock()
	devices := make([]string, 0, len(s.tokens.pushRuns))
	for deviceID := range s.tokens.pushRuns {
		devices = append(devices, deviceID)
	}
	s.tokens.mu.Unlock()
	for _, deviceID := range devices {
		if err := s.stopPushDevice(ctx, deviceID, false); err != nil {
			s.logger.Warn("push shutdown failed", "device_id", deviceID, "error", err)
		}
	}
}
