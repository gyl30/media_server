package main

import (
	"context"
	"strings"
	"time"
)

const mediaReconcileInterval = 5 * time.Second

// reconcileMedia 让信令状态跟随媒体服务器上实际运行的接收会话：
// 媒体侧已结束的直播和 RTSP 拉流被清理，信令不认识的遗留接收会话被删除。
func (s *infrastructureServer) reconcileMedia(ctx context.Context) error {
	// 先快照再查询：快照中的会话在查询前已在媒体侧创建，查询结果缺失即表示已结束。
	lives := s.live.streamingSessions()
	pulls := s.confirmedRTSPPulls()
	receivers, err := s.media.listReceivers(ctx)
	if err != nil {
		return err
	}
	running := make(map[string]struct{}, len(receivers))
	for _, receiver := range receivers {
		running[receiver.StreamID] = struct{}{}
	}

	ended := make(map[*liveSession]struct{})
	for _, session := range lives {
		if _, ok := running[session.streamID]; !ok {
			ended[session] = struct{}{}
		}
	}
	if len(ended) != 0 {
		s.live.stopMatching(ctx, func(session *liveSession) bool {
			if _, ok := ended[session]; ok {
				s.logger.Info("live ended on media server", "stream_name", session.streamName, "stream_id", session.streamID)
				return true
			}
			return false
		})
	}
	for _, pull := range pulls {
		if _, ok := running[pull.streamID]; !ok && s.dropEndedRTSPPull(pull) {
			s.logger.Info("rtsp pull ended on media server", "source_id", pull.sourceID, "stream_name", pull.streamName)
		}
	}

	// 查询之后再收集已知会话，查询前已存在且仍由信令持有的会话都不会被误删。
	known := make(map[string]struct{})
	s.live.collectStreamIDs(known)
	s.collectRTSPPullStreamIDs(known)
	for _, receiver := range receivers {
		if _, ok := known[receiver.StreamID]; ok {
			continue
		}
		s.logger.Warn("deleting orphan media receiver", "stream_name", receiver.StreamName, "stream_id", receiver.StreamID)
		var deleteErr error
		if strings.HasPrefix(receiver.StreamName, liveStreamPrefix) {
			deleteErr = s.media.deleteReceiver(ctx, receiver.StreamID, receiver.StreamName)
		} else {
			deleteErr = s.media.deleteRTSPPull(ctx, receiver.StreamID, receiver.StreamName)
		}
		if deleteErr != nil && !isMediaServerNotFound(deleteErr) {
			s.logger.Warn("orphan media receiver delete failed", "stream_name", receiver.StreamName, "error", deleteErr)
		}
	}
	return nil
}

func (s *infrastructureServer) runMediaReconcile(ctx context.Context) {
	ticker := time.NewTicker(mediaReconcileInterval)
	defer ticker.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
			reconcileContext, cancel := context.WithTimeout(ctx, mediaReconcileInterval)
			if err := s.reconcileMedia(reconcileContext); err != nil && ctx.Err() == nil {
				s.logger.Warn("media reconcile failed", "error", err)
			}
			cancel()
		}
	}
}

func (s *liveService) streamingSessions() []*liveSession {
	s.mu.Lock()
	defer s.mu.Unlock()
	var sessions []*liveSession
	for _, session := range s.sessions {
		if session.state == liveStreaming {
			sessions = append(sessions, session)
		}
	}
	return sessions
}

func (s *liveService) collectStreamIDs(ids map[string]struct{}) {
	s.mu.Lock()
	defer s.mu.Unlock()
	for _, session := range s.sessions {
		ids[session.streamID] = struct{}{}
	}
}

func (s *infrastructureServer) confirmedRTSPPulls() []rtspPullSession {
	s.rtspPullMu.Lock()
	defer s.rtspPullMu.Unlock()
	var pulls []rtspPullSession
	for _, session := range s.rtspPulls {
		if session.createConfirmed && !session.starting && session.stopDone == nil {
			pulls = append(pulls, session)
		}
	}
	return pulls
}

func (s *infrastructureServer) collectRTSPPullStreamIDs(ids map[string]struct{}) {
	s.rtspPullMu.Lock()
	defer s.rtspPullMu.Unlock()
	for _, session := range s.rtspPulls {
		ids[session.streamID] = struct{}{}
	}
}

// dropEndedRTSPPull 只移除仍处于已确认且未在停止中的同一会话，停止流程自行收尾。
func (s *infrastructureServer) dropEndedRTSPPull(expected rtspPullSession) bool {
	s.rtspPullMu.Lock()
	defer s.rtspPullMu.Unlock()
	session, ok := s.rtspPulls[expected.sourceID]
	if !ok || !sameRTSPPull(session, expected) || !session.createConfirmed || session.starting || session.stopDone != nil {
		return false
	}
	delete(s.rtspPulls, expected.sourceID)
	return true
}
