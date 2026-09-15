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
	s.sourceOperationMu.Lock()
	source, err := s.sources.setDesiredState(request.Context(), sourceID, sourceDesiredRunning)
	if err != nil {
		s.sourceOperationMu.Unlock()
		s.writeSourceError(writer, "start", sourceID, err)
		return
	}
	if runtime, ok := s.runtimes.currentForSource(sourceID); ok && runtime.State != "stopped" {
		s.sourceOperationMu.Unlock()
		writeHTTPError(writer, http.StatusConflict, "conflict")
		return
	}

	previous, hadPrevious := s.rtspSourceRuntime(sourceID)
	server := previous.server
	if !hadPrevious || !s.registry.isOnline(server) {
		server, hadPrevious = s.registry.selectOnline()
		if !hadPrevious {
			s.sourceOperationMu.Unlock()
			writeHTTPError(writer, http.StatusServiceUnavailable, "no_media_server")
			return
		}
	}
	runtime := rtspPullRuntime{
		sourceID: sourceID, streamID: uuid.NewString(), streamName: source.streamName, server: server, starting: true,
	}
	previous, hadPrevious, err = s.reserveRTSPPull(runtime)
	s.sourceOperationMu.Unlock()
	if err != nil {
		s.writeSourceRuntimeError(writer, "start", sourceID, source.streamName, err)
		return
	}
	previousGeneration, hadPreviousGeneration, previousObserved := s.runtimes.bindSource(sourceID, runtime.streamID)
	command := makeSourceRTSPPullRequest(source, runtime.streamID)
	if err := s.media.createRTSPPull(request.Context(), server, command); err != nil {
		var rejection *mediaServerHTTPRejection
		ambiguousCreate := !errors.As(err, &rejection)
		cleanupConfirmed := !ambiguousCreate
		if ambiguousCreate {
			cleanupContext, cancel := context.WithTimeout(context.Background(), s.cfg.mediaRequestTimeout)
			cleanupErr := s.media.deleteRTSPPull(cleanupContext, server, runtime.streamID, runtime.streamName)
			cancel()
			cleanupConfirmed = cleanupErr == nil
			if !cleanupConfirmed {
				s.logger.Warn("rtsp pull compensation failed", "source_id", sourceID, "stream_name", runtime.streamName,
					"server_id", server.serverID, "error", cleanupErr)
			}
		}
		if cleanupConfirmed {
			s.rollbackRTSPPull(runtime, previous, hadPrevious)
			if ambiguousCreate {
				if _, stateErr := s.runtimes.acknowledgeSourceStopped(
					server, runtime.streamID, runtime.streamName, sourceID, "rtsp"); stateErr != nil {
					s.logger.Warn("rtsp pull compensation state cleanup failed", "source_id", sourceID,
						"stream_name", runtime.streamName, "stream_id", runtime.streamID, "error", stateErr)
				}
			}
			s.runtimes.restoreSourceBinding(
				sourceID, runtime.streamID, previousGeneration, hadPreviousGeneration, previousObserved)
		} else if !s.finishUnconfirmedRTSPPull(runtime) {
			s.runtimes.restoreSourceBinding(
				sourceID, runtime.streamID, previousGeneration, hadPreviousGeneration, previousObserved)
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
			s.runtimes.restoreSourceBinding(
				sourceID, runtime.streamID, previousGeneration, hadPreviousGeneration, previousObserved)
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
	currentSource, sourceErr := s.sources.get(request.Context(), sourceID)
	if sourceErr != nil {
		if !errors.Is(sourceErr, errSourceNotFound) {
			s.writeSourceRuntimeError(writer, "start", sourceID, source.streamName, sourceErr)
			return
		}
		if s.removeRTSPPull(runtime) {
			cleanupContext, cancel := context.WithTimeout(context.Background(), s.cfg.mediaRequestTimeout)
			cleanupErr := s.media.deleteRTSPPull(cleanupContext, server, runtime.streamID, runtime.streamName)
			cancel()
			var rejection *mediaServerHTTPRejection
			if cleanupErr != nil && (!errors.As(cleanupErr, &rejection) || rejection.status != http.StatusNotFound) {
				s.logger.Warn("deleted source rtsp pull cleanup failed", "source_id", sourceID, "stream_name", runtime.streamName,
					"server_id", server.serverID, "error", cleanupErr)
			}
		}
		s.runtimes.unbindSource(sourceID, runtime.streamID)
		writeHTTPError(writer, http.StatusConflict, "conflict")
		return
	}
	if finished {
		if observed, ok := s.runtimes.currentForSource(sourceID); ok && observed.StreamID == runtime.streamID && observed.State == "stopped" {
			s.removeRTSPPull(runtime)
		} else if currentSource.desiredState == sourceDesiredStopped {
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
	for {
		s.sourceOperationMu.Lock()
		if _, err := s.sources.setDesiredState(ctx, sourceID, sourceDesiredStopped); err != nil {
			s.sourceOperationMu.Unlock()
			return err
		}
		runtime, wait, owner, err := s.beginRTSPPullStop(sourceID)
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
		deleteErr := s.media.deleteRTSPPull(ctx, runtime.server, runtime.streamID, runtime.streamName)
		if deleteErr != nil {
			var rejection *mediaServerHTTPRejection
			if runtime.createConfirmed && errors.As(deleteErr, &rejection) && rejection.status == http.StatusNotFound {
				deleteErr = nil
			}
		}
		if deleteErr != nil {
			s.finishRTSPPullStop(runtime, false)
			return deleteErr
		}
		_, err = s.runtimes.acknowledgeSourceStopped(
			runtime.server, runtime.streamID, runtime.streamName, runtime.sourceID, "rtsp")
		s.finishRTSPPullStop(runtime, true)
		return err
	}
}

func (s *infrastructureServer) writeSourceRuntimeError(writer http.ResponseWriter, operation, sourceID, streamName string, err error) {
	if errors.Is(err, errRTSPPullStarting) || errors.Is(err, errRTSPPullStopping) || errors.Is(err, errRTSPPullStreamChanged) {
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
