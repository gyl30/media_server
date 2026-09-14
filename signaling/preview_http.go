package main

import (
	"encoding/json"
	"net"
	"net/http"
	"net/url"
	"strconv"

	"github.com/google/uuid"
)

type previewStartRequest struct {
	SourceID  json.RawMessage `json:"source_id"`
	DeviceID  json.RawMessage `json:"device_id"`
	ChannelID json.RawMessage `json:"channel_id"`
}

type previewTarget struct {
	sourceID  string
	deviceID  string
	channelID string
}

type previewStartResponse struct {
	StreamID string `json:"stream_id"`
	WHEPURL  string `json:"whep_url"`
}

func (s *infrastructureServer) handlePreviewStart(writer http.ResponseWriter, request *http.Request) {
	var command previewStartRequest
	if !decodeJSON(writer, request, &command) {
		writeHTTPError(writer, http.StatusBadRequest, "invalid_request")
		return
	}
	target, valid := makePreviewTarget(command)
	if !valid {
		writeHTTPError(writer, http.StatusBadRequest, "invalid_request")
		return
	}

	var streamName string
	var server mediaServerInstance
	if target.sourceID != "" {
		if _, err := s.sources.get(request.Context(), target.sourceID); err != nil {
			s.writeSourceError(writer, "preview", target.sourceID, err)
			return
		}
		runtime, ok := s.rtspSourceRuntime(target.sourceID)
		if !ok {
			writeHTTPError(writer, http.StatusConflict, "not_running")
			return
		}
		streamName, server = runtime.streamName, runtime.server
	} else {
		if s.live == nil {
			writeHTTPError(writer, http.StatusConflict, "not_running")
			return
		}
		live, ok := s.live.live(target.deviceID, target.channelID)
		if !ok || live.state == liveStopping {
			writeHTTPError(writer, http.StatusConflict, "not_running")
			return
		}
		streamName, server = live.streamName, live.server
	}
	if !s.registry.isOnline(server) {
		writeHTTPError(writer, http.StatusServiceUnavailable, "no_media_server")
		return
	}

	writeJSON(writer, http.StatusCreated, previewStartResponse{
		StreamID: uuid.NewString(), WHEPURL: makeWHEPURL(streamName, server),
	})
}

func makePreviewTarget(command previewStartRequest) (previewTarget, bool) {
	sourceID, sourceValid := decodeOptionalString(command.SourceID)
	deviceID, deviceValid := decodeOptionalString(command.DeviceID)
	channelID, channelValid := decodeOptionalString(command.ChannelID)
	if !sourceValid || !deviceValid || !channelValid ||
		(sourceID != nil && *sourceID == "") || (deviceID != nil && *deviceID == "") || (channelID != nil && *channelID == "") {
		return previewTarget{}, false
	}
	target := previewTarget{}
	if sourceID != nil {
		target.sourceID = *sourceID
	}
	if deviceID != nil {
		target.deviceID = *deviceID
	}
	if channelID != nil {
		target.channelID = *channelID
	}
	sourceTarget := target.sourceID != ""
	gbTarget := target.deviceID != "" || target.channelID != ""
	if sourceTarget == gbTarget {
		return previewTarget{}, false
	}
	if sourceTarget {
		return target, validUUIDv4(target.sourceID)
	}
	return target, validDigits(target.deviceID, 20) && validDigits(target.channelID, 20)
}

func makeWHEPURL(streamName string, server mediaServerInstance) string {
	path := "/play/whep/" + streamName
	endpoint := url.URL{
		Scheme:  "http",
		Host:    net.JoinHostPort(server.mediaIP, strconv.FormatUint(uint64(server.httpPort), 10)),
		Path:    path,
		RawPath: "/play/whep/" + url.PathEscape(streamName),
	}
	return endpoint.String()
}
