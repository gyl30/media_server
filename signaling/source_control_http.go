package main

import (
	"context"
	"errors"
	"net/http"

	"github.com/google/uuid"
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
	source, err := s.sources.setDesiredState(request.Context(), sourceID, sourceDesiredRunning)
	if err != nil {
		s.writeSourceError(writer, "start", sourceID, err)
		return
	}
	if runtime, ok := s.runtimes.currentForSource(sourceID); ok && runtime.State != "stopped" {
		writeHTTPError(writer, http.StatusConflict, "conflict")
		return
	}

	previous, hadPrevious := s.rtspSourceRuntime(sourceID)
	server := previous.server
	if !hadPrevious || !s.registry.isOnline(server) {
		server, hadPrevious = s.registry.selectOnline()
		if !hadPrevious {
			writeHTTPError(writer, http.StatusServiceUnavailable, "no_media_server")
			return
		}
	}
	runtime := rtspPullRuntime{
		sourceID: sourceID, streamID: uuid.NewString(), streamName: source.streamName, server: server, starting: true,
	}
	previous, hadPrevious, err = s.reserveRTSPPull(runtime)
	if err != nil {
		s.writeSourceRuntimeError(writer, "start", sourceID, source.streamName, err)
		return
	}
	previousGeneration, hadPreviousGeneration := s.runtimes.bindSource(sourceID, runtime.streamID)
	command := makeSourceRTSPPullRequest(source, runtime.streamID)
	if err := s.media.createRTSPPull(request.Context(), server, command); err != nil {
		var rejection *mediaServerHTTPRejection
		if !errors.As(err, &rejection) {
			cleanupContext, cancel := context.WithTimeout(context.Background(), s.cfg.mediaRequestTimeout)
			cleanupErr := s.media.deleteRTSPPull(cleanupContext, server, runtime.streamID, runtime.streamName)
			cancel()
			if cleanupErr != nil && (!errors.As(cleanupErr, &rejection) || rejection.status != http.StatusNotFound) {
				s.logger.Warn("rtsp pull compensation failed", "source_id", sourceID, "stream_name", runtime.streamName,
					"server_id", server.serverID, "error", cleanupErr)
			}
		}
		if s.rollbackRTSPPull(runtime, previous, hadPrevious) {
			s.runtimes.restoreSourceBinding(sourceID, runtime.streamID, previousGeneration, hadPreviousGeneration)
		}
		s.writeSourceRuntimeError(writer, "start", sourceID, source.streamName, err)
		return
	}
	finished := s.finishRTSPPull(runtime)
	if !s.registry.isOnline(server) {
		if finished {
			s.removeRTSPPull(runtime)
		}
		if observed, ok := s.runtimes.currentForSource(sourceID); !ok || observed.StreamID != runtime.streamID {
			s.runtimes.restoreSourceBinding(sourceID, runtime.streamID, previousGeneration, hadPreviousGeneration)
		}
		cleanupContext, cancel := context.WithTimeout(context.Background(), s.cfg.mediaRequestTimeout)
		cleanupErr := s.media.deleteRTSPPull(cleanupContext, server, runtime.streamID, runtime.streamName)
		cancel()
		var rejection *mediaServerHTTPRejection
		if cleanupErr != nil && (!errors.As(cleanupErr, &rejection) || rejection.status != http.StatusNotFound) {
			s.logger.Warn("offline rtsp pull cleanup failed", "source_id", sourceID, "stream_name", runtime.streamName,
				"server_id", server.serverID, "error", cleanupErr)
		}
		writeHTTPError(writer, http.StatusServiceUnavailable, "no_media_server")
		return
	}
	if finished {
		if observed, ok := s.runtimes.currentForSource(sourceID); ok && observed.StreamID == runtime.streamID && observed.State == "stopped" {
			s.removeRTSPPull(runtime)
		} else if current, err := s.sources.get(request.Context(), sourceID); err == nil && current.desiredState == sourceDesiredStopped {
			if err := s.stopSource(request.Context(), sourceID); err != nil {
				s.writeSourceRuntimeError(writer, "stop", sourceID, source.streamName, err)
				return
			}
			writeHTTPError(writer, http.StatusConflict, "conflict")
			return
		}
	}
	writeJSON(writer, http.StatusCreated, map[string]string{"result": "ok", "stream_id": runtime.streamID})
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
		s.writeSourceRuntimeError(writer, "stop", sourceID, "", err)
		return
	}
	writeJSON(writer, http.StatusOK, map[string]string{"result": "ok"})
}

func (s *infrastructureServer) stopSource(ctx context.Context, sourceID string) error {
	if _, err := s.sources.setDesiredState(ctx, sourceID, sourceDesiredStopped); err != nil {
		return err
	}
	runtime, ok, err := s.takeRTSPPull(sourceID)
	if err != nil || !ok {
		return err
	}
	if err := s.media.deleteRTSPPull(ctx, runtime.server, runtime.streamID, runtime.streamName); err != nil {
		var rejection *mediaServerHTTPRejection
		if !errors.As(err, &rejection) || rejection.status != http.StatusNotFound {
			s.restoreRTSPPull(runtime)
			return err
		}
	}
	_, err = s.runtimes.acknowledgeSourceStopped(
		runtime.server, runtime.streamID, runtime.streamName, runtime.sourceID, "rtsp")
	return err
}

func (s *infrastructureServer) writeSourceRuntimeError(writer http.ResponseWriter, operation, sourceID, streamName string, err error) {
	if errors.Is(err, errRTSPPullStarting) || errors.Is(err, errRTSPPullStreamChanged) {
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
	s.logger.Error("source runtime operation failed", "operation", operation, "source_id", sourceID,
		"stream_name", streamName, "error", err)
	writeHTTPError(writer, http.StatusBadGateway, "source_"+operation+"_failed")
}

func makeSourceRTSPPullRequest(source rtspSource, streamID string) rtspPullCreateRequest {
	command := rtspPullCreateRequest{
		StreamID: streamID, SourceID: &source.sourceID, StreamName: source.streamName, URL: source.url,
	}
	if source.username != "" {
		command.Username = &source.username
		command.Password = &source.password
	}
	return command
}
