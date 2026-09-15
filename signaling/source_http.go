package main

import (
	"encoding/json"
	"errors"
	"net/http"
	"net/url"
	"strconv"
	"strings"

	"github.com/google/uuid"
)

type sourceCreateRequest struct {
	StreamName string          `json:"stream_name"`
	URL        string          `json:"url"`
	Username   json.RawMessage `json:"username"`
	Password   json.RawMessage `json:"password"`
}

type sourcePatchRequest struct {
	StreamName json.RawMessage `json:"stream_name"`
	URL        json.RawMessage `json:"url"`
	Username   json.RawMessage `json:"username"`
	Password   json.RawMessage `json:"password"`
}

type sourceResponse struct {
	SourceID     string             `json:"source_id"`
	StreamName   string             `json:"stream_name"`
	URL          string             `json:"url"`
	Username     string             `json:"username"`
	DesiredState sourceDesiredState `json:"desired_state"`
	Observed     *observedRuntime   `json:"observed,omitempty"`
}

func (s *infrastructureServer) handleSourceList(writer http.ResponseWriter, request *http.Request) {
	sources, err := s.sources.list(request.Context())
	if err != nil {
		s.logger.Error("list sources failed", "error", err)
		writeHTTPError(writer, http.StatusInternalServerError, "operation_failed")
		return
	}
	response := make([]sourceResponse, 0, len(sources))
	for _, source := range sources {
		response = append(response, s.makeSourceResponse(source))
	}
	writeJSON(writer, http.StatusOK, map[string]any{"sources": response})
}

func (s *infrastructureServer) handleSourceCreate(writer http.ResponseWriter, request *http.Request) {
	var payload sourceCreateRequest
	if !decodeJSON(writer, request, &payload) || payload.StreamName == "" || !validRTSPSourceURL(payload.URL) {
		writeHTTPError(writer, http.StatusBadRequest, "invalid_request")
		return
	}
	username, usernameValid := decodeOptionalString(payload.Username)
	password, passwordValid := decodeOptionalString(payload.Password)
	if !usernameValid || !passwordValid || (password != nil && (username == nil || *username == "")) {
		writeHTTPError(writer, http.StatusBadRequest, "invalid_request")
		return
	}
	source := rtspSource{
		sourceID: uuid.NewString(), streamName: payload.StreamName, url: payload.URL,
		desiredState: sourceDesiredStopped,
	}
	if username != nil {
		source.username = *username
	}
	if password != nil {
		source.password = *password
	}
	if err := s.sources.create(request.Context(), source); err != nil {
		s.writeSourceError(writer, "create", source.sourceID, err)
		return
	}
	writeJSON(writer, http.StatusCreated, s.makeSourceResponse(source))
}

func (s *infrastructureServer) handleSourcePatch(writer http.ResponseWriter, request *http.Request) {
	sourceID := request.PathValue("source_id")
	var payload sourcePatchRequest
	if !validUUIDv4(sourceID) || !decodeJSON(writer, request, &payload) ||
		(payload.StreamName == nil && payload.URL == nil && payload.Username == nil && payload.Password == nil) {
		writeHTTPError(writer, http.StatusBadRequest, "invalid_request")
		return
	}
	streamName, streamNameValid := decodeOptionalString(payload.StreamName)
	urlValue, urlValid := decodeOptionalString(payload.URL)
	username, usernameValid := decodeOptionalString(payload.Username)
	password, passwordValid := decodeOptionalString(payload.Password)
	if !streamNameValid || !urlValid || !usernameValid || !passwordValid ||
		(streamName != nil && *streamName == "") || (urlValue != nil && !validRTSPSourceURL(*urlValue)) {
		writeHTTPError(writer, http.StatusBadRequest, "invalid_request")
		return
	}
	source, err := s.sources.patch(request.Context(), sourceID, rtspSourcePatch{
		streamName: streamName, url: urlValue, username: username, password: password,
	})
	if err != nil {
		s.writeSourceError(writer, "patch", sourceID, err)
		return
	}
	writeJSON(writer, http.StatusOK, s.makeSourceResponse(source))
}

func (s *infrastructureServer) handleSourceDelete(writer http.ResponseWriter, request *http.Request) {
	if !s.beginSourceControl() {
		writeHTTPError(writer, http.StatusServiceUnavailable, "server_shutdown")
		return
	}
	defer s.endSourceControl()

	sourceID := request.PathValue("source_id")
	if !validUUIDv4(sourceID) {
		writeHTTPError(writer, http.StatusBadRequest, "invalid_request")
		return
	}
	if err := s.stopSource(request.Context(), sourceID); err != nil {
		s.writeSourceRuntimeError(writer, "delete", sourceID, "", err)
		return
	}
	currentStreamID, hasCurrent := s.runtimes.sourceBinding(sourceID)
	if err := s.sources.deleteStopped(request.Context(), sourceID); err != nil {
		s.writeSourceError(writer, "delete", sourceID, err)
		return
	}
	if hasCurrent {
		s.runtimes.unbindSource(sourceID, currentStreamID)
	}
	writeJSON(writer, http.StatusOK, map[string]string{"result": "ok"})
}

func (s *infrastructureServer) writeSourceError(writer http.ResponseWriter, operation, sourceID string, err error) {
	switch {
	case errors.Is(err, errInvalidSource):
		writeHTTPError(writer, http.StatusBadRequest, "invalid_request")
	case errors.Is(err, errSourceNotFound):
		writeHTTPError(writer, http.StatusNotFound, "not_found")
	case errors.Is(err, errSourceConflict):
		writeHTTPError(writer, http.StatusConflict, "conflict")
	default:
		s.logger.Error("source operation failed", "operation", operation, "source_id", sourceID, "error", err)
		writeHTTPError(writer, http.StatusInternalServerError, "operation_failed")
	}
}

func (s *infrastructureServer) makeSourceResponse(source rtspSource) sourceResponse {
	response := sourceResponse{
		SourceID: source.sourceID, StreamName: source.streamName, URL: source.url,
		Username: source.username, DesiredState: source.desiredState,
	}
	if runtime, ok := s.runtimes.currentForSource(source.sourceID); ok {
		response.Observed = &runtime
	}
	return response
}

func validRTSPSourceURL(value string) bool {
	parsed, err := url.Parse(value)
	if err != nil || parsed.Scheme != "rtsp" || parsed.Host == "" || parsed.Hostname() == "" || parsed.User != nil {
		return false
	}
	if strings.HasSuffix(parsed.Host, ":") {
		return false
	}
	if parsed.Port() == "" {
		return true
	}
	port, err := strconv.ParseUint(parsed.Port(), 10, 16)
	return err == nil && port != 0
}
