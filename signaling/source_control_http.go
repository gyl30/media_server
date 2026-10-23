package main

import (
	"context"
	"errors"
	"net/http"
	"strings"
)

func (s *infrastructureServer) handleSourceStart(writer http.ResponseWriter, request *http.Request) {
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
	streamID, err := s.startSource(request.Context(), sourceID)
	if err != nil {
		s.writeSourceSessionError(writer, "start", sourceID, "", err)
		return
	}
	writeJSON(writer, http.StatusCreated, map[string]string{"stream_id": streamID})
}

func (s *infrastructureServer) startSource(ctx context.Context, sourceID string) (string, error) {
	s.sourceOperationMu.Lock()
	defer s.sourceOperationMu.Unlock()
	source, err := s.sources.setDesiredState(ctx, sourceID, sourceDesiredRunning)
	if err != nil {
		return "", err
	}
	session, err := s.reserveRTSPPull(sourceID, source.streamName)
	if err != nil {
		return "", err
	}

	command := pullCreateRequest{
		StreamID: session.streamID, URL: source.url,
	}
	switch {
	case strings.HasPrefix(source.url, string(pullProtocolWHEP)+"://") && source.password != "":
		command.Token = &source.password
	case source.username != "":
		command.Username = &source.username
		command.Password = &source.password
	}
	if err := s.media.createPull(ctx, command); err != nil {
		s.removeRTSPPull(sourceID)
		return "", err
	}
	s.finishRTSPPull(sourceID)
	s.logger.Info("rtsp pull started", "source_id", sourceID, "stream_name", session.streamName, "stream_id", session.streamID)
	return session.streamID, nil
}

func (s *infrastructureServer) resumeSources(ctx context.Context) {
	if !s.beginSourceControl() {
		return
	}
	defer s.endSourceControl()
	sources, err := s.sources.list(ctx)
	if err != nil {
		s.logger.Warn("resume sources failed", "error", err)
		return
	}
	for _, source := range sources {
		if source.desiredState != sourceDesiredRunning {
			continue
		}
		if _, err := s.startSource(ctx, source.sourceID); err != nil {
			s.logger.Warn("resume source failed", "source_id", source.sourceID, "stream_name", source.streamName, "error", err)
		}
	}
}

func (s *infrastructureServer) handleSourceStop(writer http.ResponseWriter, request *http.Request) {
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
		s.writeSourceSessionError(writer, "stop", sourceID, "", err)
		return
	}
	writer.WriteHeader(http.StatusNoContent)
}

func (s *infrastructureServer) stopSource(ctx context.Context, sourceID string, desired sourceDesiredState) error {
	s.sourceOperationMu.Lock()
	defer s.sourceOperationMu.Unlock()
	if _, err := s.sources.setDesiredState(ctx, sourceID, desired); err != nil {
		return err
	}
	session, ok := s.removeRTSPPull(sourceID)
	if !ok {
		return nil
	}
	if err := s.media.deleteReceiver(ctx, session.streamID); err != nil && !isMediaServerNotFound(err) {
		return err
	}
	s.logger.Info("rtsp pull stopped", "source_id", sourceID, "stream_name", session.streamName, "stream_id", session.streamID)
	return nil
}

func (s *infrastructureServer) writeSourceSessionError(writer http.ResponseWriter, operation, sourceID, streamName string, err error) {
	if errors.Is(err, errRTSPPullExists) {
		writeHTTPError(writer, http.StatusConflict, "conflict")
		return
	}
	var rejection *mediaServerHTTPRejection
	if errors.As(err, &rejection) {
		switch {
		case rejection.status == http.StatusBadRequest && rejection.code == "invalid_request":
			writeHTTPError(writer, http.StatusBadRequest, "invalid_request")
			return
		case rejection.status == http.StatusConflict && rejection.code == "conflict":
			writeHTTPError(writer, http.StatusConflict, "conflict")
			return
		}
	}
	if errors.Is(err, errSourceNotFound) || errors.Is(err, errInvalidSource) || errors.Is(err, errSourceConflict) {
		s.writeSourceError(writer, operation, sourceID, err)
		return
	}
	s.logger.Error("source session operation failed", "operation", operation, "source_id", sourceID,
		"stream_name", streamName, "error", err)
	writeHTTPError(writer, http.StatusBadGateway, "source_"+operation+"_failed")
}
