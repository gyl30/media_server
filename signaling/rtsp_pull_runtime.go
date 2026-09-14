package main

import (
	"context"
	"errors"
	"sync"
)

var (
	errRTSPPullStarting      = errors.New("RTSP pull is starting")
	errRTSPPullStreamChanged = errors.New("RTSP pull stream name changed")
)

type rtspPullRuntime struct {
	sourceID   string
	streamID   string
	streamName string
	server     mediaServerInstance
	starting   bool
}

func (s *infrastructureServer) rtspSourceRuntime(sourceID string) (rtspPullRuntime, bool) {
	s.rtspPullMu.Lock()
	runtime, ok := s.rtspPulls[sourceID]
	s.rtspPullMu.Unlock()
	return runtime, ok
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

func (s *infrastructureServer) reserveRTSPPull(runtime rtspPullRuntime) (rtspPullRuntime, bool, error) {
	s.rtspPullMu.Lock()
	defer s.rtspPullMu.Unlock()
	previous, exists := s.rtspPulls[runtime.sourceID]
	if exists && previous.starting {
		return rtspPullRuntime{}, false, errRTSPPullStarting
	}
	if exists && previous.streamName != runtime.streamName {
		return rtspPullRuntime{}, false, errRTSPPullStreamChanged
	}
	s.rtspPulls[runtime.sourceID] = runtime
	return previous, exists, nil
}

func (s *infrastructureServer) finishRTSPPull(expected rtspPullRuntime) bool {
	s.rtspPullMu.Lock()
	defer s.rtspPullMu.Unlock()
	runtime, ok := s.rtspPulls[expected.sourceID]
	if !ok || !sameRTSPPull(runtime, expected) {
		return false
	}
	runtime.starting = false
	s.rtspPulls[expected.sourceID] = runtime
	return true
}

func (s *infrastructureServer) rollbackRTSPPull(expected, previous rtspPullRuntime, hadPrevious bool) bool {
	s.rtspPullMu.Lock()
	defer s.rtspPullMu.Unlock()
	runtime, ok := s.rtspPulls[expected.sourceID]
	if !ok || !sameRTSPPull(runtime, expected) {
		return false
	}
	if hadPrevious {
		s.rtspPulls[expected.sourceID] = previous
		return true
	}
	delete(s.rtspPulls, expected.sourceID)
	return true
}

func (s *infrastructureServer) takeRTSPPull(sourceID string) (rtspPullRuntime, bool, error) {
	s.rtspPullMu.Lock()
	defer s.rtspPullMu.Unlock()
	runtime, ok := s.rtspPulls[sourceID]
	if !ok {
		return rtspPullRuntime{}, false, nil
	}
	if runtime.starting {
		return rtspPullRuntime{}, false, errRTSPPullStarting
	}
	delete(s.rtspPulls, sourceID)
	return runtime, true, nil
}

func (s *infrastructureServer) restoreRTSPPull(runtime rtspPullRuntime) {
	s.rtspPullMu.Lock()
	if _, exists := s.rtspPulls[runtime.sourceID]; !exists {
		s.rtspPulls[runtime.sourceID] = runtime
	}
	s.rtspPullMu.Unlock()
}

func (s *infrastructureServer) removeRTSPPull(expected rtspPullRuntime) bool {
	if expected.sourceID == "" {
		return false
	}
	s.rtspPullMu.Lock()
	defer s.rtspPullMu.Unlock()
	runtime, ok := s.rtspPulls[expected.sourceID]
	if !ok || !sameRTSPPull(runtime, expected) {
		return false
	}
	delete(s.rtspPulls, expected.sourceID)
	return true
}

func (s *infrastructureServer) removeRTSPPullsForMediaServer(expected mediaServerInstance) {
	s.rtspPullMu.Lock()
	for sourceID, runtime := range s.rtspPulls {
		if runtime.server.serverID == expected.serverID && runtime.server.instanceID == expected.instanceID {
			delete(s.rtspPulls, sourceID)
		}
	}
	s.rtspPullMu.Unlock()
}

func (s *infrastructureServer) shutdownRTSPPulls(ctx context.Context) {
	s.sourceControlMu.Lock()
	s.sourceControlClosed = true
	s.sourceControlMu.Unlock()
	s.sourceControlWait.Wait()

	s.rtspPullMu.Lock()
	pulls := s.rtspPulls
	s.rtspPulls = make(map[string]rtspPullRuntime)
	s.rtspPullMu.Unlock()

	var wait sync.WaitGroup
	for _, runtime := range pulls {
		wait.Add(1)
		go func() {
			defer wait.Done()
			if err := s.media.deleteRTSPPull(ctx, runtime.server, runtime.streamID, runtime.streamName); err != nil {
				s.logger.Warn("rtsp pull shutdown failed", "stream_name", runtime.streamName,
					"source_id", runtime.sourceID, "server_id", runtime.server.serverID, "error", err)
			}
		}()
	}
	wait.Wait()
}

func sameRTSPPull(left, right rtspPullRuntime) bool {
	return left.sourceID == right.sourceID && left.streamID == right.streamID && left.streamName == right.streamName &&
		left.server.serverID == right.server.serverID && left.server.instanceID == right.server.instanceID
}
