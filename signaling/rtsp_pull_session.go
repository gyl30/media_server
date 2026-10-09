package main

import (
	"context"
	"errors"
	"sync"

	"github.com/google/uuid"
)

var (
	errRTSPPullExists     = errors.New("RTSP pull already exists")
	errRTSPPullStarting   = errors.New("RTSP pull is starting")
	errRTSPPullStopping   = errors.New("RTSP pull is stopping")
	errRTSPPullUnresolved = errors.New("RTSP pull ownership is unresolved")
)

type rtspPullSession struct {
	sourceID        string
	streamID        string
	streamName      string
	starting        bool
	createConfirmed bool
	stopDone        chan struct{}
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
	if current, exists := s.rtspPulls[sourceID]; exists {
		switch {
		case current.starting:
			return rtspPullSession{}, errRTSPPullStarting
		case current.stopDone != nil:
			return rtspPullSession{}, errRTSPPullStopping
		case !current.createConfirmed:
			return rtspPullSession{}, errRTSPPullUnresolved
		default:
			return rtspPullSession{}, errRTSPPullExists
		}
	}
	session := rtspPullSession{
		sourceID: sourceID, streamID: uuid.NewString(), streamName: streamName, starting: true,
	}
	s.rtspPulls[sourceID] = session
	return session, nil
}

func (s *infrastructureServer) finishRTSPPull(expected rtspPullSession) bool {
	s.rtspPullMu.Lock()
	defer s.rtspPullMu.Unlock()
	session, ok := s.rtspPulls[expected.sourceID]
	if !ok || !sameRTSPPull(session, expected) {
		return false
	}
	session.starting = false
	session.createConfirmed = true
	s.rtspPulls[expected.sourceID] = session
	return true
}

func (s *infrastructureServer) finishUnconfirmedRTSPPull(expected rtspPullSession) bool {
	s.rtspPullMu.Lock()
	defer s.rtspPullMu.Unlock()
	session, ok := s.rtspPulls[expected.sourceID]
	if !ok || !sameRTSPPull(session, expected) {
		return false
	}
	session.starting = false
	s.rtspPulls[expected.sourceID] = session
	return true
}

func (s *infrastructureServer) removeRTSPPull(expected rtspPullSession) bool {
	if expected.sourceID == "" {
		return false
	}
	s.rtspPullMu.Lock()
	defer s.rtspPullMu.Unlock()
	session, ok := s.rtspPulls[expected.sourceID]
	if !ok || !sameRTSPPull(session, expected) {
		return false
	}
	delete(s.rtspPulls, expected.sourceID)
	if session.stopDone != nil {
		close(session.stopDone)
	}
	return true
}

func (s *infrastructureServer) beginRTSPPullStop(sourceID string) (rtspPullSession, <-chan struct{}, bool, error) {
	s.rtspPullMu.Lock()
	defer s.rtspPullMu.Unlock()
	session, ok := s.rtspPulls[sourceID]
	if !ok {
		return rtspPullSession{}, nil, false, nil
	}
	if session.starting {
		return rtspPullSession{}, nil, false, errRTSPPullStarting
	}
	if session.stopDone != nil {
		return rtspPullSession{}, session.stopDone, false, nil
	}
	session.stopDone = make(chan struct{})
	s.rtspPulls[sourceID] = session
	return session, nil, true, nil
}

func (s *infrastructureServer) finishRTSPPullStop(expected rtspPullSession, succeeded bool) bool {
	s.rtspPullMu.Lock()
	defer s.rtspPullMu.Unlock()
	session, ok := s.rtspPulls[expected.sourceID]
	if !ok || !sameRTSPPull(session, expected) || session.stopDone != expected.stopDone {
		return false
	}
	if succeeded {
		delete(s.rtspPulls, expected.sourceID)
	} else {
		session.stopDone = nil
		s.rtspPulls[expected.sourceID] = session
	}
	close(expected.stopDone)
	return true
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
		if session.stopDone != nil {
			close(session.stopDone)
		}
	}
	s.rtspPullMu.Unlock()

	var wait sync.WaitGroup
	for _, session := range pulls {
		wait.Add(1)
		go func() {
			defer wait.Done()
			if err := s.media.deleteReceiver(ctx, session.streamID, session.streamName); err != nil {
				s.logger.Warn("rtsp pull shutdown failed", "stream_name", session.streamName,
					"source_id", session.sourceID, "error", err)
			}
		}()
	}
	wait.Wait()
}

func sameRTSPPull(left, right rtspPullSession) bool {
	return left.sourceID == right.sourceID && left.streamID == right.streamID && left.streamName == right.streamName
}
