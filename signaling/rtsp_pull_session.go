package main

import (
	"context"
	"errors"
	"sync"

	"github.com/google/uuid"
)

var errRTSPPullExists = errors.New("RTSP pull already exists")

type rtspPullSession struct {
	sourceID   string
	streamID   string
	streamName string
	starting   bool
}

func (s *infrastructureServer) rtspSourceSession(sourceID string) (rtspPullSession, bool) {
	s.rtspPullMu.Lock()
	session, ok := s.rtspPulls[sourceID]
	s.rtspPullMu.Unlock()
	return session, ok
}

func (s *infrastructureServer) beginSourceControl() bool {
	s.sourceControlMu.Lock()
	defer s.sourceControlMu.Unlock()
	if s.sourceControlClosed {
		return false
	}
	s.sourceControlWait.Add(1)
	return true
}

func (s *infrastructureServer) endSourceControl() {
	s.sourceControlWait.Done()
}

func (s *infrastructureServer) reserveRTSPPull(sourceID, streamName string) (rtspPullSession, error) {
	s.rtspPullMu.Lock()
	defer s.rtspPullMu.Unlock()
	if _, exists := s.rtspPulls[sourceID]; exists {
		return rtspPullSession{}, errRTSPPullExists
	}
	session := rtspPullSession{
		sourceID: sourceID, streamID: uuid.NewString(), streamName: streamName, starting: true,
	}
	s.rtspPulls[sourceID] = session
	return session, nil
}

func (s *infrastructureServer) finishRTSPPull(sourceID string) {
	s.rtspPullMu.Lock()
	defer s.rtspPullMu.Unlock()
	session := s.rtspPulls[sourceID]
	session.starting = false
	s.rtspPulls[sourceID] = session
}

func (s *infrastructureServer) removeRTSPPull(sourceID string) (rtspPullSession, bool) {
	s.rtspPullMu.Lock()
	defer s.rtspPullMu.Unlock()
	session, ok := s.rtspPulls[sourceID]
	if !ok {
		return rtspPullSession{}, false
	}
	delete(s.rtspPulls, sourceID)
	s.tokens.revokeStream(session.streamID)
	return session, true
}

func (s *infrastructureServer) shutdownRTSPPulls(ctx context.Context) {
	s.sourceControlMu.Lock()
	s.sourceControlClosed = true
	s.sourceControlMu.Unlock()
	s.sourceControlWait.Wait()

	s.rtspPullMu.Lock()
	pulls := s.rtspPulls
	s.rtspPulls = make(map[string]rtspPullSession)
	for _, session := range pulls {
		s.tokens.revokeStream(session.streamID)
	}
	s.rtspPullMu.Unlock()

	var wait sync.WaitGroup
	for _, session := range pulls {
		wait.Add(1)
		go func() {
			defer wait.Done()
			if err := s.media.deleteReceiver(ctx, session.streamID); err != nil {
				s.logger.Warn("rtsp pull shutdown failed", "stream_name", session.streamName,
					"source_id", session.sourceID, "error", err)
			}
		}()
	}
	wait.Wait()
}
