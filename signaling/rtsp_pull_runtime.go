package main

import (
	"context"
	"errors"
	"sync"

	"github.com/google/uuid"
)

var (
	errRTSPPullStarting      = errors.New("RTSP pull is starting")
	errRTSPPullStopping      = errors.New("RTSP pull is stopping")
	errRTSPPullUnresolved    = errors.New("RTSP pull ownership is unresolved")
	errRTSPPullStreamChanged = errors.New("RTSP pull stream name changed")
)

type rtspPullRuntime struct {
	sourceID        string
	streamID        string
	streamName      string
	server          mediaServerInstance
	starting        bool
	createConfirmed bool
	stopDone        chan struct{}
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

func (s *infrastructureServer) reserveRTSPPull(runtime rtspPullRuntime) (rtspPullRuntime, rtspPullRuntime, bool, error) {
	s.rtspPullMu.Lock()
	defer s.rtspPullMu.Unlock()
	previous, exists := s.rtspPulls[runtime.sourceID]
	if exists && previous.starting {
		return rtspPullRuntime{}, rtspPullRuntime{}, false, errRTSPPullStarting
	}
	if exists && previous.stopDone != nil {
		return rtspPullRuntime{}, rtspPullRuntime{}, false, errRTSPPullStopping
	}
	if exists && !previous.createConfirmed {
		return rtspPullRuntime{}, rtspPullRuntime{}, false, errRTSPPullUnresolved
	}
	if exists && previous.streamName != runtime.streamName {
		return rtspPullRuntime{}, rtspPullRuntime{}, false, errRTSPPullStreamChanged
	}
	if !s.registry.isOnline(runtime.server) {
		return rtspPullRuntime{}, rtspPullRuntime{}, false, errMediaServerStale
	}
	runtime.streamID = uuid.NewString()
	s.rtspPulls[runtime.sourceID] = runtime
	return runtime, previous, exists, nil
}

func (s *infrastructureServer) finishRTSPPull(expected rtspPullRuntime) bool {
	s.rtspPullMu.Lock()
	defer s.rtspPullMu.Unlock()
	runtime, ok := s.rtspPulls[expected.sourceID]
	if !ok || !sameRTSPPull(runtime, expected) {
		return false
	}
	runtime.starting = false
	runtime.createConfirmed = true
	s.rtspPulls[expected.sourceID] = runtime
	return true
}

func (s *infrastructureServer) finishUnconfirmedRTSPPull(expected rtspPullRuntime) bool {
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

func (s *infrastructureServer) confirmRTSPPull(expected rtspPullRuntime) bool {
	s.rtspPullMu.Lock()
	defer s.rtspPullMu.Unlock()
	runtime, ok := s.rtspPulls[expected.sourceID]
	if !ok || !sameRTSPPull(runtime, expected) {
		return false
	}
	runtime.createConfirmed = true
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

func (s *infrastructureServer) beginRTSPPullStop(sourceID string) (rtspPullRuntime, <-chan struct{}, bool, error) {
	s.rtspPullMu.Lock()
	defer s.rtspPullMu.Unlock()
	runtime, ok := s.rtspPulls[sourceID]
	if !ok {
		return rtspPullRuntime{}, nil, false, nil
	}
	if runtime.starting {
		return rtspPullRuntime{}, nil, false, errRTSPPullStarting
	}
	if runtime.stopDone != nil {
		return rtspPullRuntime{}, runtime.stopDone, false, nil
	}
	runtime.stopDone = make(chan struct{})
	s.rtspPulls[sourceID] = runtime
	return runtime, nil, true, nil
}

func (s *infrastructureServer) finishRTSPPullStop(expected rtspPullRuntime, succeeded bool) bool {
	s.rtspPullMu.Lock()
	defer s.rtspPullMu.Unlock()
	runtime, ok := s.rtspPulls[expected.sourceID]
	if !ok || !sameRTSPPull(runtime, expected) || runtime.stopDone != expected.stopDone {
		return false
	}
	if succeeded {
		delete(s.rtspPulls, expected.sourceID)
	} else {
		runtime.stopDone = nil
		s.rtspPulls[expected.sourceID] = runtime
	}
	close(expected.stopDone)
	return true
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
	if runtime.stopDone != nil {
		close(runtime.stopDone)
	}
	return true
}

func (s *infrastructureServer) removeRTSPPullsForMediaServer(expected mediaServerInstance) {
	s.rtspPullMu.Lock()
	for sourceID, runtime := range s.rtspPulls {
		if runtime.server.serverID == expected.serverID && runtime.server.instanceID == expected.instanceID {
			delete(s.rtspPulls, sourceID)
			if runtime.stopDone != nil {
				close(runtime.stopDone)
			}
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
	for _, runtime := range pulls {
		if runtime.stopDone != nil {
			close(runtime.stopDone)
		}
	}
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
