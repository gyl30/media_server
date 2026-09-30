package main

import (
	"encoding/json"
	"net"
	"net/http"
	"net/url"
	"strconv"
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
	if target.sourceID != "" {
		if _, err := s.sources.get(request.Context(), target.sourceID); err != nil {
			s.writeSourceError(writer, "preview", target.sourceID, err)
			return
		}
		session, ok := s.rtspSourceSession(target.sourceID)
		if !ok || session.starting || !session.createConfirmed || session.stopDone != nil {
			writeHTTPError(writer, http.StatusConflict, "not_running")
			return
		}
		streamName = session.streamName
	} else {
		live, ok := s.live.live(target.deviceID, target.channelID)
		if !ok || live.state == liveStopping {
			writeHTTPError(writer, http.StatusConflict, "not_running")
			return
		}
		streamName = live.streamName
	}

	writeJSON(writer, http.StatusCreated, map[string]string{
		"whep_url": makeWHEPURL(streamName, s.media.server),
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

func makeWHEPURL(streamName string, server mediaServer) string {
	path := "/play/whep/" + streamName
	endpoint := url.URL{
		Scheme:  "http",
		Host:    net.JoinHostPort(server.mediaIP, strconv.FormatUint(uint64(server.httpPort), 10)),
		Path:    path,
		RawPath: "/play/whep/" + url.PathEscape(streamName),
	}
	return endpoint.String()
}
