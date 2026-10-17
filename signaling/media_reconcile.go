package main

import (
	"context"
	"maps"
	"time"
)

const mediaReconcileInterval = 5 * time.Second
const pushRegistrationGrace = 3 * time.Second

// reconcileMedia 让信令状态跟随媒体服务器上实际运行的接收会话：
// 媒体侧已结束的直播和 RTSP 拉流被清理，信令不认识的遗留接收会话被删除。
func (s *infrastructureServer) reconcileMedia(ctx context.Context) error {
	// 运行上限由历史 context 表达；即使媒体 HTTP 不可达，也先请求已到期 owner 结束。
	s.live.stopMatching(ctx, func(session *liveSession) bool {
		if session.key.historyID != "" && session.state == liveStreaming && session.ctx.Err() != nil {
			s.logger.Warn("playback application deadline reached; transfer may be incomplete", "stream_id", session.streamID, "error", session.ctx.Err())
			return true
		}
		return false
	})
	// 音频不在 receiver 清单中。先续其控制租约，避免视频查询失败阻断 SIP owner 的清理。
	s.live.mu.Lock()
	var broadcasts []*broadcastSession
	for _, session := range s.live.broadcasts {
		if session.local.rtp.Port() != 0 && session.ctx.Err() == nil {
			broadcasts = append(broadcasts, session)
		}
	}
	s.live.mu.Unlock()
	for _, session := range broadcasts {
		err := s.media.post(ctx, s.media.server.controlURL+"/gb28181/audio/status", map[string]string{"session_id": session.mediaID}, 204, nil)
		if err == nil {
			continue
		}
		s.live.mu.Lock()
		if s.live.broadcasts[session.deviceID] == session {
			session.cancel(err)
		}
		s.live.mu.Unlock()
		// 无法维持租约也结束本次对讲，但不把网络未知误报为设备离线或媒体已不存在。
		s.logger.Warn("talk control lease failed", "talk_id", session.mediaID, "error", err)
	}
	now := s.live.sip.now()
	// 先快照再查询：快照中的会话在查询前已在媒体侧创建，查询结果缺失即表示已结束。
	lives := s.live.streamingSessions()
	pulls := s.confirmedRTSPPulls()
	s.tokens.mu.Lock()
	pushes := make(map[string]pushRun)
	for deviceID, run := range s.tokens.pushRuns {
		if now.Sub(run.verifiedAt) > pushRegistrationGrace {
			pushes[deviceID] = run
		}
	}
	s.tokens.mu.Unlock()
	receivers, err := s.media.listReceivers(ctx)
	if err != nil {
		return err
	}
	running := make(map[string]struct{}, len(receivers))
	ready := make(map[string]struct{})
	for _, receiver := range receivers {
		running[receiver.StreamID] = struct{}{}
		if receiver.Ready {
			ready[receiver.StreamID] = struct{}{}
		}
	}
	s.readyStreams.Store(&ready)

	ended := make(map[*liveSession]struct{})
	for _, session := range lives {
		if _, ok := running[session.streamID]; !ok {
			ended[session] = struct{}{}
		}
	}
	if len(ended) != 0 {
		s.live.stopMatching(ctx, func(session *liveSession) bool {
			if _, ok := ended[session]; ok {
				s.logger.Info("live ended on media server", "stream_id", session.streamID)
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
	s.tokens.mu.Lock()
	for deviceID, expected := range pushes {
		if _, exists := running[expected.streamID]; exists {
			continue
		}
		if current, exists := s.tokens.pushRuns[deviceID]; exists && current.streamID == expected.streamID {
			delete(s.tokens.pushRuns, deviceID)
			maps.DeleteFunc(s.tokens.tokens, func(_ string, token streamToken) bool { return token.streamID == expected.streamID })
			s.logger.Info("push ended on media server", "device_id", deviceID, "stream_id", expected.streamID)
		}
	}
	s.tokens.mu.Unlock()

	// 查询之后再收集已知会话，查询前已存在且仍由信令持有的会话都不会被误删。
	known := make(map[string]struct{})
	s.live.collectStreamIDs(known)
	s.collectRTSPPullStreamIDs(known)
	s.tokens.mu.Lock()
	for _, run := range s.tokens.pushRuns {
		known[run.streamID] = struct{}{}
	}
	s.tokens.mu.Unlock()
	for _, receiver := range receivers {
		if _, ok := known[receiver.StreamID]; ok {
			continue
		}
		s.logger.Warn("deleting orphan media receiver", "stream_id", receiver.StreamID)
		deleteErr := s.media.deleteReceiver(ctx, receiver.StreamID)
		if deleteErr != nil && !isMediaServerNotFound(deleteErr) {
			s.logger.Warn("orphan media receiver delete failed", "stream_id", receiver.StreamID, "error", deleteErr)
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
		if session.state == liveStreaming || (session.key.historyID != "" && session.state == liveStopping) {
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
	s.tokens.revokeStream(session.streamID)
	return true
}
