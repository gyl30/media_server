package main

import (
	"context"
	"errors"
	"net/http"
	"strings"
	"syscall"
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

// startSource 把期望状态设为 running 并在媒体服务器上建立拉流会话；调用方已进入 source control。
func (s *infrastructureServer) startSource(ctx context.Context, sourceID string) (string, error) {
	s.sourceOperationMu.Lock()
	source, err := s.sources.setDesiredState(ctx, sourceID, sourceDesiredRunning)
	if err != nil {
		s.sourceOperationMu.Unlock()
		return "", err
	}
	session, err := s.reserveRTSPPull(sourceID, source.streamName)
	s.sourceOperationMu.Unlock()
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
		// 媒体服务器明确拒绝或拒绝连接时请求一定没有生效；其他错误（超时、连接中断）结果未知，需要补偿删除。
		var rejection *mediaServerHTTPRejection
		ambiguousCreate := !errors.As(err, &rejection) && !errors.Is(err, syscall.ECONNREFUSED)
		cleanupConfirmed := !ambiguousCreate
		if ambiguousCreate {
			cleanupContext, cancel := s.media.timeoutContext()
			cleanupErr := s.media.deleteReceiver(cleanupContext, session.streamID)
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
		return "", err
	}
	if !s.finishRTSPPull(session) {
		cleanupContext, cancel := s.media.timeoutContext()
		cleanupErr := s.media.deleteReceiver(cleanupContext, session.streamID)
		cancel()
		if cleanupErr != nil && !isMediaServerNotFound(cleanupErr) {
			s.logger.Warn("orphan rtsp pull cleanup failed", "source_id", sourceID, "stream_name", session.streamName, "error", cleanupErr)
		}
		return "", errRTSPPullStopping
	}

	currentSource, sourceErr := s.sources.get(ctx, sourceID)
	if sourceErr != nil {
		if !errors.Is(sourceErr, errSourceNotFound) {
			return "", sourceErr
		}
		if s.removeRTSPPull(session) {
			cleanupContext, cancel := s.media.timeoutContext()
			cleanupErr := s.media.deleteReceiver(cleanupContext, session.streamID)
			cancel()
			if cleanupErr != nil && !isMediaServerNotFound(cleanupErr) {
				s.logger.Warn("deleted source rtsp pull cleanup failed", "source_id", sourceID, "stream_name", session.streamName, "error", cleanupErr)
			}
		}
		return "", errSourceConflict
	}
	if currentSource.desiredState == sourceDesiredStopped {
		if err := s.stopSource(ctx, sourceID, sourceDesiredStopped); err != nil {
			return "", err
		}
		return "", errSourceConflict
	}
	return session.streamID, nil
}

// resumeSources 在启动时按持久化的期望状态恢复拉流；失败只记录，界面显示连接失败并允许重试。
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

// stopSource 结束当前拉流会话并记录期望状态；修改配置后重新拉流时期望状态保持 running。
func (s *infrastructureServer) stopSource(ctx context.Context, sourceID string, desired sourceDesiredState) error {
	for {
		s.sourceOperationMu.Lock()
		if _, err := s.sources.setDesiredState(ctx, sourceID, desired); err != nil {
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
		deleteErr := s.media.deleteReceiver(ctx, session.streamID)
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
