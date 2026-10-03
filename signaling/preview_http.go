package main

import (
	"encoding/json"
	"net"
	"net/http"
	"net/url"
	"strconv"
)

type previewStartRequest struct {
	SourceID json.RawMessage `json:"source_id"`
}

func (s *infrastructureServer) handlePreviewStart(writer http.ResponseWriter, request *http.Request) {
	var command previewStartRequest
	if !decodeJSON(writer, request, &command) {
		writeHTTPError(writer, http.StatusBadRequest, "invalid_request")
		return
	}
	sourceID, valid := decodeOptionalString(command.SourceID)
	if !valid || sourceID == nil || !validUUIDv4(*sourceID) {
		writeHTTPError(writer, http.StatusBadRequest, "invalid_request")
		return
	}
	if _, err := s.sources.get(request.Context(), *sourceID); err != nil {
		s.writeSourceError(writer, "preview", *sourceID, err)
		return
	}
	session, ok := s.rtspSourceSession(*sourceID)
	if !ok || session.starting || !session.createConfirmed || session.stopDone != nil {
		writeHTTPError(writer, http.StatusConflict, "not_running")
		return
	}
	writeJSON(writer, http.StatusCreated, map[string]string{
		"whep_url": makeWHEPURL(session.streamName, s.media.server),
	})
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
