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
	DeviceID   string       `json:"device_id"`
	Name       string       `json:"name"`
	Protocol   pushProtocol `json:"protocol"`
	State      string       `json:"state"`
	ExpiresAt  time.Time    `json:"expires_at,omitzero"`
	UsedAt     time.Time    `json:"used_at,omitzero"`
	MediaReady bool         `json:"media_ready"`
	pushURLs
}

type pushURLs struct {
	Token   string `json:"token,omitempty"`
	RTMPURL string `json:"rtmp_url,omitempty"`
	RTSPURL string `json:"rtsp_url,omitempty"`
	WHIPURL string `json:"whip_url,omitempty"`
}

func (s *infrastructureServer) pushURLs(credential string) pushURLs {
	server := s.media.server
	return pushURLs{
		Token:   credential,
		RTMPURL: (&url.URL{Scheme: "rtmp", Host: net.JoinHostPort(server.mediaIP, strconv.Itoa(int(server.rtmpPort))), Path: "/live/" + credential}).String(),
		RTSPURL: (&url.URL{Scheme: "rtsp", Host: net.JoinHostPort(server.mediaIP, strconv.Itoa(int(server.rtspPort))), Path: "/" + credential}).String(),
		WHIPURL: server.publicHTTPOrigin() + "/publish/whip/" + credential,
	}
}

func (s *infrastructureServer) pushDeviceResponseLocked(device pushDevice, now time.Time) pushDeviceResponse {
	response := pushDeviceResponse{
		DeviceID: device.deviceID, Name: device.name, Protocol: device.protocol,
		ExpiresAt: device.expiresAt, UsedAt: device.usedAt, pushURLs: s.pushURLs(device.credential),
	}
	switch run, running := s.tokens.pushRuns[device.deviceID]; {
	case running && run.streamID == device.credential:
		response.State = "publishing"
		response.MediaReady = s.mediaReady(run.streamID)
	case !device.usedAt.IsZero():
		response.State = "used"
	case !now.Before(device.expiresAt):
		response.State = "expired"
	default:
		response.State = "pending"
	}
	return response
}

func (s *infrastructureServer) writePushDevice(writer http.ResponseWriter, request *http.Request, deviceID string, status int) {
	device, err := s.sources.getPushDevice(request.Context(), deviceID)
	if err != nil {
		s.writeDeviceError(writer, err)
		return
	}
	s.tokens.mu.Lock()
	response := s.pushDeviceResponseLocked(device, s.live.sip.now())
	s.tokens.mu.Unlock()
	writer.Header().Set("Cache-Control", "no-store")
	writeJSON(writer, status, response)
}

func (s *infrastructureServer) handlePushDeviceList(writer http.ResponseWriter, request *http.Request) {
	devices, err := s.sources.listPushDevices(request.Context())
	if err != nil {
		s.writeDeviceError(writer, err)
		return
	}
	now := s.live.sip.now()
	s.tokens.mu.Lock()
	response := make([]pushDeviceResponse, 0, len(devices))
	for _, device := range devices {
		response = append(response, s.pushDeviceResponseLocked(device, now))
	}
	s.tokens.mu.Unlock()
	writer.Header().Set("Cache-Control", "no-store")
	writeJSON(writer, http.StatusOK, map[string]any{"devices": response})
}

func (s *infrastructureServer) handlePushCredentialPrepare(writer http.ResponseWriter, request *http.Request) {
	value, err := newStreamToken()
	if err != nil {
		writeHTTPError(writer, http.StatusInternalServerError, "operation_failed")
		return
	}
	now := s.live.sip.now()
	s.tokens.mu.Lock()
	s.tokens.expireLocked(now)
	s.tokens.prepared[value] = now.Add(pushCredentialTTL)
	s.tokens.mu.Unlock()
	writer.Header().Set("Cache-Control", "no-store")
	writeJSON(writer, http.StatusCreated, s.pushURLs(value))
}

func (s *infrastructureServer) handlePushDeviceCreate(writer http.ResponseWriter, request *http.Request) {
	var command struct {
		Name     string       `json:"name"`
		Protocol pushProtocol `json:"protocol"`
		Token    string       `json:"token"`
	}
	if !decodeJSON(writer, request, &command) {
		writeHTTPError(writer, http.StatusBadRequest, "invalid_request")
		return
	}
	device := pushDevice{deviceID: uuid.NewString(), name: command.Name, protocol: command.Protocol, credential: command.Token}
	s.tokens.mu.Lock()
	now := s.live.sip.now()
	s.tokens.expireLocked(now)
	if _, ok := s.tokens.prepared[device.credential]; !ok {
		s.tokens.mu.Unlock()
		writeHTTPError(writer, http.StatusBadRequest, "invalid_credential")
		return
	}
	device.expiresAt = pushCredentialDeadline(now)
	err := s.sources.createPushDevice(request.Context(), device)
	if err == nil {
		delete(s.tokens.prepared, device.credential)
	}
	response := s.pushDeviceResponseLocked(device, now)
	s.tokens.mu.Unlock()
	if err != nil {
		s.writeDeviceError(writer, err)
		return
	}
	s.logger.Info("push device created", "device_id", device.deviceID, "protocol", device.protocol, "expires_at", device.expiresAt)
	writer.Header().Set("Cache-Control", "no-store")
	writeJSON(writer, http.StatusCreated, response)
}

func (s *infrastructureServer) handlePushDeviceGet(writer http.ResponseWriter, request *http.Request) {
	deviceID := request.PathValue("id")
	if !validUUIDv4(deviceID) {
		writeHTTPError(writer, http.StatusBadRequest, "invalid_request")
		return
	}
	s.writePushDevice(writer, request, deviceID, http.StatusOK)
}

func (s *infrastructureServer) handlePushDevicePatch(writer http.ResponseWriter, request *http.Request) {
	deviceID := request.PathValue("id")
	var command struct {
		Name     string        `json:"name"`
		Protocol *pushProtocol `json:"protocol"`
	}
	if !validUUIDv4(deviceID) || !decodeJSON(writer, request, &command) {
		writeHTTPError(writer, http.StatusBadRequest, "invalid_request")
		return
	}
	if err := s.sources.patchPushDevice(request.Context(), deviceID, command.Name, command.Protocol); err != nil {
		s.writeDeviceError(writer, err)
		return
	}
	s.writePushDevice(writer, request, deviceID, http.StatusOK)
}

func (s *infrastructureServer) handlePushPublish(writer http.ResponseWriter, request *http.Request) {
	deviceID := request.PathValue("id")
	if !validUUIDv4(deviceID) {
		writeHTTPError(writer, http.StatusBadRequest, "invalid_request")
		return
	}
	value, err := newStreamToken()
	if err != nil {
		writeHTTPError(writer, http.StatusInternalServerError, "operation_failed")
		return
	}
	s.sourceOperationMu.Lock()
	s.tokens.mu.Lock()
	if _, publishing := s.tokens.pushRuns[deviceID]; publishing {
		s.tokens.mu.Unlock()
		s.sourceOperationMu.Unlock()
		writeHTTPError(writer, http.StatusConflict, "publishing")
		return
	}
	expiresAt := pushCredentialDeadline(s.live.sip.now())
	err = s.sources.renewPushCredential(request.Context(), deviceID, value, expiresAt)
	s.tokens.mu.Unlock()
	s.sourceOperationMu.Unlock()
	if err != nil {
		s.writeDeviceError(writer, err)
		return
	}
	s.logger.Info("push credential renewed", "device_id", deviceID, "expires_at", expiresAt)
	s.writePushDevice(writer, request, deviceID, http.StatusCreated)
}

func (s *infrastructureServer) stopPushDevice(ctx context.Context, deviceID string, remove bool) (err error) {
	operation := "stop"
	if remove {
		operation = "delete"
	}
	defer func() {
		if err != nil {
			s.logger.Warn("push device operation failed", "operation", operation, "device_id", deviceID, "error", err)
		} else {
			s.logger.Info("push device operation completed", "operation", operation, "device_id", deviceID)
		}
	}()
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
