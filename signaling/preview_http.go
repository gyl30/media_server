package main

import (
	"encoding/json"
	"net/http"
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
	result, err := s.newPlaybackURLs(playSource{SourceID: *sourceID})
	if err != nil {
		writeHTTPError(writer, http.StatusConflict, "not_running")
		return
	}
	writer.Header().Set("Cache-Control", "no-store")
	writeJSON(writer, http.StatusCreated, result)
}
