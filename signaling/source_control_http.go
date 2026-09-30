package main

import (
	"context"
	"errors"
	"net/http"
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
	s.sourceOperationMu.Lock()
	source, err := s.sources.setDesiredState(request.Context(), sourceID, sourceDesiredRunning)
	if err != nil {
		s.sourceOperationMu.Unlock()
		s.writeSourceError(writer, "start", sourceID, err)
		return
	}
	session, err := s.reserveRTSPPull(sourceID, source.streamName)
	s.sourceOperationMu.Unlock()
	if err != nil {
		s.writeSourceSessionError(writer, "start", sourceID, source.streamName, err)
		return
	}

	command := makeSourceRTSPPullRequest(source, session.streamID)
	if err := s.media.createRTSPPull(request.Context(), command); err != nil {
		var rejection *mediaServerHTTPRejection
		ambiguousCreate := !errors.As(err, &rejection)
		cleanupConfirmed := !ambiguousCreate
		if ambiguousCreate {
			cleanupContext, cancel := s.media.timeoutContext()
			cleanupErr := s.media.deleteRTSPPull(cleanupContext, session.streamID, session.streamName)
			cancel()
			cleanupConfirmed = cleanupErr == nil || isMediaServerNotFound(cleanupErr)
			if !cleanupConfirmed {
				s.logger.Warn("rtsp pull compensation failed", "source_id", sourceID, "stream_name", session.streamName, "error", cleanupErr)
			}
		}
		if cleanupConfirmed {
			s.removeRTSPPull(session)
		} else {
			s.finishUnconfirmedRTSPPull(session)
		}
		s.writeSourceSessionError(writer, "start", sourceID, source.streamName, err)
		return
	}
	if !s.finishRTSPPull(session) {
		cleanupContext, cancel := s.media.timeoutContext()
		cleanupErr := s.media.deleteRTSPPull(cleanupContext, session.streamID, session.streamName)
		cancel()
		if cleanupErr != nil && !isMediaServerNotFound(cleanupErr) {
			s.logger.Warn("orphan rtsp pull cleanup failed", "source_id", sourceID, "stream_name", session.streamName, "error", cleanupErr)
		}
		writeHTTPError(writer, http.StatusConflict, "conflict")
		return
	}

	currentSource, sourceErr := s.sources.get(request.Context(), sourceID)
	if sourceErr != nil {
		if !errors.Is(sourceErr, errSourceNotFound) {
			s.writeSourceSessionError(writer, "start", sourceID, source.streamName, sourceErr)
			return
		}
		if s.removeRTSPPull(session) {
			cleanupContext, cancel := s.media.timeoutContext()
			cleanupErr := s.media.deleteRTSPPull(cleanupContext, session.streamID, session.streamName)
			cancel()
			if cleanupErr != nil && !isMediaServerNotFound(cleanupErr) {
				s.logger.Warn("deleted source rtsp pull cleanup failed", "source_id", sourceID, "stream_name", session.streamName, "error", cleanupErr)
			}
		}
		writeHTTPError(writer, http.StatusConflict, "conflict")
		return
	}
	if currentSource.desiredState == sourceDesiredStopped {
		if err := s.stopSource(request.Context(), sourceID); err != nil {
			s.writeSourceSessionError(writer, "stop", sourceID, source.streamName, err)
			return
		}
		writeHTTPError(writer, http.StatusConflict, "conflict")
		return
	}
	writeJSON(writer, http.StatusCreated, map[string]string{"stream_id": session.streamID})
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
	if err := s.stopSource(request.Context(), sourceID); err != nil {
		s.writeSourceSessionError(writer, "stop", sourceID, "", err)
		return
	}
	writer.WriteHeader(http.StatusNoContent)
}

func (s *infrastructureServer) stopSource(ctx context.Context, sourceID string) error {
	for {
		s.sourceOperationMu.Lock()
		if _, err := s.sources.setDesiredState(ctx, sourceID, sourceDesiredStopped); err != nil {
			s.sourceOperationMu.Unlock()
			return err
		}
		session, wait, owner, err := s.beginRTSPPullStop(sourceID)
		s.sourceOperationMu.Unlock()
		if err != nil || (!owner && wait == nil) {
			return err
		}
		if !owner {
			select {
			case <-wait:
				continue
			case <-ctx.Done():
				return ctx.Err()
			}
		}
		deleteErr := s.media.deleteRTSPPull(ctx, session.streamID, session.streamName)
		if isMediaServerNotFound(deleteErr) {
			deleteErr = nil
		}
		s.finishRTSPPullStop(session, deleteErr == nil)
		return deleteErr
	}
}

func (s *infrastructureServer) writeSourceSessionError(writer http.ResponseWriter, operation, sourceID, streamName string, err error) {
	if errors.Is(err, errRTSPPullExists) || errors.Is(err, errRTSPPullStarting) || errors.Is(err, errRTSPPullStopping) ||
		errors.Is(err, errRTSPPullUnresolved) {
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

func isMediaServerNotFound(err error) bool {
	var rejection *mediaServerHTTPRejection
	return errors.As(err, &rejection) && rejection.status == http.StatusNotFound
}

func makeSourceRTSPPullRequest(source rtspSource, streamID string) rtspPullCreateRequest {
	command := rtspPullCreateRequest{
		StreamID: streamID, SourceID: source.sourceID, StreamName: source.streamName, URL: source.url,
	}
	if source.username != "" {
		command.Username = &source.username
		command.Password = &source.password
	}
	return command
}
