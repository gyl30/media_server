package main

import (
	"bytes"
	"errors"
	"io"
	"mime"
	"net"
	"net/http"
	"net/url"
	"strconv"
	"strings"
	"time"
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
	ticket, err := s.live.newPlayTicket(deviceID, channelID, view.streamID)
	if err != nil {
		writeHTTPError(writer, http.StatusConflict, "live_stopping")
		return
	}
	scheme := "http"
	if request.TLS != nil {
		scheme = "https"
	}
	endpoint := url.URL{Scheme: scheme, Host: request.Host, Path: "/play/whep/" + ticket.playID}
	writer.Header().Set("Cache-Control", "no-store")
	writeJSON(writer, http.StatusCreated, map[string]any{
		"live_id": ticket.liveID, "play_id": ticket.playID,
		"expires_at": ticket.expiresAt, "whep_url": endpoint.String(),
	})
}

func (s *infrastructureServer) handlePlayWHEP(writer http.ResponseWriter, request *http.Request) {
	writer.Header().Set("Access-Control-Allow-Origin", "*")
	writer.Header().Set("Cache-Control", "no-store")
	if request.Method == http.MethodOptions {
		writer.Header().Set("Access-Control-Allow-Methods", "POST, OPTIONS")
		writer.Header().Set("Access-Control-Allow-Headers", "Content-Type")
		writer.Header().Set("Accept-Post", "application/sdp")
		writer.WriteHeader(http.StatusOK)
		return
	}
	ticket, session, ok := s.live.takePlayTicket(request.PathValue("play_id"))
	if !ok {
		writeHTTPError(writer, http.StatusNotFound, "play_not_found")
		return
	}
	defer session.offers.Done()
	mediaType, _, err := mime.ParseMediaType(request.Header.Get("Content-Type"))
	if err != nil || !strings.EqualFold(mediaType, "application/sdp") {
		writeHTTPError(writer, http.StatusUnsupportedMediaType, "invalid_content_type")
		return
	}
	controller := http.NewResponseController(writer)
	_ = controller.SetReadDeadline(time.Now().Add(s.media.client.Timeout))
	_ = controller.SetWriteDeadline(time.Now().Add(2 * s.media.client.Timeout))
	body, err := io.ReadAll(http.MaxBytesReader(writer, request.Body, 1024*1024))
	if err != nil {
		writeHTTPError(writer, http.StatusBadRequest, "invalid_offer")
		return
	}
	endpoint := s.media.server.controlURL + "/play/whep/" + ticket.liveID
	offer, err := http.NewRequestWithContext(request.Context(), http.MethodPost, endpoint, bytes.NewReader(body))
	if err != nil {
		writeHTTPError(writer, http.StatusBadGateway, "whep_failed")
		return
	}
	offer.Header.Set("Content-Type", "application/sdp")
	response, err := s.media.client.Do(offer)
	if err != nil {
		writeHTTPError(writer, http.StatusBadGateway, "whep_unavailable")
		return
	}
	defer response.Body.Close()
	answer, err := io.ReadAll(io.LimitReader(response.Body, 1024*1024+1))
	if err != nil || len(answer) > 1024*1024 {
		writeHTTPError(writer, http.StatusBadGateway, "invalid_whep_response")
		return
	}
	for _, header := range []string{"Content-Type", "Cache-Control", "Access-Control-Allow-Origin", "Access-Control-Expose-Headers"} {
		if value := response.Header.Get(header); value != "" {
			writer.Header().Set(header, value)
		}
	}
	if location := response.Header.Get("Location"); location != "" {
		resource, err := url.Parse(location)
		if err != nil || !strings.HasPrefix(resource.Path, "/play/whep/session/") {
			writeHTTPError(writer, http.StatusBadGateway, "invalid_whep_response")
			return
		}
		resource.Scheme = "http"
		resource.Host = net.JoinHostPort(s.media.server.mediaIP, strconv.Itoa(int(s.media.server.httpPort)))
		writer.Header().Set("Location", resource.String())
	}
	writer.WriteHeader(response.StatusCode)
	_, _ = writer.Write(answer)
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
