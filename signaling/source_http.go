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
	SourceID     string                 `json:"source_id"`
	StreamName   string                 `json:"stream_name"`
	URL          string                 `json:"url"`
	Username     string                 `json:"username"`
	PasswordSet  bool                   `json:"password_set"`
	DesiredState sourceDesiredState     `json:"desired_state"`
	Session      *sourceSessionResponse `json:"session,omitempty"`
}

type sourceSessionResponse struct {
	StreamID   string `json:"stream_id"`
	State      string `json:"state"`
	MediaReady bool   `json:"media_ready"`
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
	if !s.beginSourceControl() {
		writeHTTPError(writer, http.StatusServiceUnavailable, "server_shutdown")
		return
	}
	defer s.endSourceControl()

	var payload sourceCreateRequest
	if !decodeJSON(writer, request, &payload) || payload.StreamName == "" || !validPullSourceURL(payload.URL) {
		writeHTTPError(writer, http.StatusBadRequest, "invalid_request")
		return
	}
	username, usernameValid := decodeOptionalString(payload.Username)
	password, passwordValid := decodeOptionalString(payload.Password)
	if !usernameValid || !passwordValid {
		writeHTTPError(writer, http.StatusBadRequest, "invalid_request")
		return
	}
	source := rtspSource{
		sourceID: uuid.NewString(), streamName: payload.StreamName, url: payload.URL,
		desiredState: sourceDesiredRunning,
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
	if _, err := s.startSource(request.Context(), source.sourceID); err != nil {
		s.logger.Warn("source start after create failed", "source_id", source.sourceID, "stream_name", source.streamName, "error", err)
	}
	writeJSON(writer, http.StatusCreated, s.makeSourceResponse(source))
}

func (s *infrastructureServer) handleSourcePatch(writer http.ResponseWriter, request *http.Request) {
	if !s.beginSourceControl() {
		writeHTTPError(writer, http.StatusServiceUnavailable, "server_shutdown")
		return
	}
	defer s.endSourceControl()

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
		(streamName != nil && *streamName == "") || (urlValue != nil && !validPullSourceURL(*urlValue)) {
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
	if source.desiredState == sourceDesiredRunning && (urlValue != nil || username != nil || password != nil) {
		if err := s.stopSource(request.Context(), sourceID, sourceDesiredRunning); err != nil {
			s.writeSourceSessionError(writer, "patch", sourceID, source.streamName, err)
			return
		}
		if _, err := s.startSource(request.Context(), sourceID); err != nil {
			s.logger.Warn("source restart after patch failed", "source_id", sourceID, "stream_name", source.streamName, "error", err)
		}
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
	if err := s.stopSource(request.Context(), sourceID, sourceDesiredStopped); err != nil {
		s.writeSourceSessionError(writer, "delete", sourceID, "", err)
		return
	}
	s.sourceOperationMu.Lock()
	err := s.sources.deleteStopped(request.Context(), sourceID)
	s.sourceOperationMu.Unlock()
	if err != nil {
		s.writeSourceError(writer, "delete", sourceID, err)
		return
	}
	writer.WriteHeader(http.StatusNoContent)
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
		Username: source.username, PasswordSet: source.password != "", DesiredState: source.desiredState,
	}
	if session, ok := s.rtspSourceSession(source.sourceID); ok {
		state := "created"
		if session.starting {
			state = "starting"
		}
		response.Session = &sourceSessionResponse{StreamID: session.streamID, State: state, MediaReady: state == "created" && s.mediaReady(session.streamID)}
	}
	return response
}

type pullProtocol string

const (
	pullProtocolRTSP pullProtocol = "rtsp"
	pullProtocolRTMP pullProtocol = "rtmp"
	pullProtocolWHEP pullProtocol = "http"
)

func validPullSourceURL(value string) bool {
	parsed, err := url.Parse(value)
	if err != nil || parsed.Host == "" || parsed.Hostname() == "" || parsed.User != nil {
		return false
	}
	switch pullProtocol(parsed.Scheme) {
	case pullProtocolRTSP, pullProtocolWHEP:
	case pullProtocolRTMP:
		app, stream, found := strings.Cut(strings.TrimPrefix(parsed.Path, "/"), "/")
		if !found || app == "" || stream == "" {
			return false
		}
	default:
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

func validUUIDv4(value string) bool {
	parsed, err := uuid.Parse(value)
	return err == nil && parsed.Version() == 4 && parsed.Variant() == uuid.RFC4122 && parsed.String() == value
}
