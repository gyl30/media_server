package main

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"math"
	"net"
	"net/http"
	"net/url"
	"strconv"
	"strings"
	"time"

	"github.com/emiago/sipgo"
	"github.com/emiago/sipgo/sip"
	"github.com/google/uuid"
)

// 这是 1x 回放的应用资源上限，不是设备已交付全部录像的保证。
const playbackCompletionGrace = time.Minute
const playbackDrainTimeout = 20 * time.Second
const playbackControlTimeout = 5 * time.Second

var (
	errPlaybackNotStreaming = errors.New("playback is not streaming")
	errPlaybackControlBusy  = errors.New("playback control is in progress")
	errPlaybackRejected     = errors.New("device rejected playback control")
)

type playbackTicket struct {
	PlaybackID string    `json:"playback_id"`
	PSURL      string    `json:"ps_url"`
	ExpiresAt  time.Time `json:"expires_at"`
	Deadline   time.Time `json:"deadline"`
}

// historyMediaSeconds 是按倍速传完录像所需的整秒数，向上取整且不做加法溢出。
func historyMediaSeconds(startTime, endTime int64, speed int) int64 {
	duration := endTime - startTime
	seconds := duration / int64(speed)
	if duration%int64(speed) != 0 {
		seconds++
	}
	return seconds
}

// handleHistoryCreate 创建历史回放或下载实例；下载要求 speed 为 1/2/4，回放不接受 speed。
func (s *infrastructureServer) handleHistoryCreate(writer http.ResponseWriter, request *http.Request, session gbSession) {
	var command struct {
		StartTime int64 `json:"start_time"`
		EndTime   int64 `json:"end_time"`
		// 区分字段缺失与 0/null：回放只要出现 speed 就拒绝。
		Speed json.RawMessage `json:"speed"`
	}
	deviceID, channelID := request.PathValue("device_id"), request.PathValue("channel_id")
	var speed int
	if !validDigits(deviceID, 20) || !validDigits(channelID, 20) || !decodeJSON(writer, request, &command) || command.StartTime <= 0 || command.EndTime <= command.StartTime ||
		(session == sessionPlayback && command.Speed != nil) ||
		(session == sessionDownload && (json.Unmarshal(command.Speed, &speed) != nil || !validDownloadSpeed(speed))) {
		writeHTTPError(writer, http.StatusBadRequest, "invalid_request")
		return
	}
	if session == sessionPlayback {
		speed = 1
	}
	if historyMediaSeconds(command.StartTime, command.EndTime, speed) > int64((time.Duration(math.MaxInt64)-s.tokenTTL-playbackCompletionGrace)/time.Second) {
		writeHTTPError(writer, http.StatusBadRequest, "invalid_request")
		return
	}
	result, err := s.startPlayback(request.Context(), deviceID, channelID, command.StartTime, command.EndTime, session, speed)
	if err != nil {
		switch {
		case errors.Is(err, errDeviceNotFound):
			writeHTTPError(writer, http.StatusNotFound, "device_not_found")
		case errors.Is(err, errChannelNotFound):
			writeHTTPError(writer, http.StatusNotFound, "channel_not_found")
		case errors.Is(err, errDeviceOffline):
			writeHTTPError(writer, http.StatusConflict, "device_offline")
		case errors.Is(err, errDeviceStopping):
			writeHTTPError(writer, http.StatusConflict, "device_stopping")
		default:
			s.logger.Error("playback prepare failed", "playback_id", result.PlaybackID, "error", err)
			failure := map[string]string{"error": "playback_start_failed"}
			s.live.mu.Lock()
			if session := s.live.sessions[liveKey{deviceID: deviceID, channelID: channelID, historyID: result.PlaybackID}]; session != nil && session.state == liveCleanupPending {
				failure["playback_id"] = result.PlaybackID
			}
			s.live.mu.Unlock()
			writeJSON(writer, http.StatusBadGateway, failure)
		}
		return
	}
	writer.Header().Set("Cache-Control", "no-store")
	writer.Header().Set("Content-Type", "application/json")
	writer.WriteHeader(http.StatusCreated)
	if err := json.NewEncoder(writer).Encode(result); err != nil || request.Context().Err() != nil {
		cleanupContext, cancel := s.media.timeoutContext()
		defer cancel()
		_ = s.live.stopPlaybackID(cleanupContext, result.PlaybackID)
	}
}

func (s *infrastructureServer) startPlayback(ctx context.Context, deviceID, channelID string, startTime, endTime int64, purpose gbSession, speed int) (playbackTicket, error) {
	live := s.live
	live.mu.Lock()
	if _, err := live.sip.deviceStore.get(ctx, deviceID); err != nil {
		live.mu.Unlock()
		return playbackTicket{}, err
	}
	if _, stopping := live.stoppingDevices[deviceID]; stopping {
		live.mu.Unlock()
		return playbackTicket{}, errDeviceStopping
	}
	if _, online := live.sip.devices.getOnline(deviceID, live.sip.now()); !online {
		live.mu.Unlock()
		return playbackTicket{}, errDeviceOffline
	}
	if _, exists := live.sip.channels.get(deviceID, channelID); !exists {
		live.mu.Unlock()
		return playbackTicket{}, errChannelNotFound
	}
	ssrc, err := live.ssrcs.acquire(playbackReceiver)
	if err != nil {
		live.mu.Unlock()
		return playbackTicket{}, err
	}
	operationContext, cancel := context.WithTimeout(context.Background(), time.Duration(historyMediaSeconds(startTime, endTime, speed))*time.Second+s.tokenTTL+playbackCompletionGrace)
	id := uuid.NewString()
	session := &liveSession{
		key: liveKey{deviceID: deviceID, channelID: channelID, historyID: id}, streamID: id, ssrc: ssrc,
		state: livePreparing, ctx: operationContext, cancel: cancel, claimed: make(chan struct{}), established: make(chan struct{}), done: make(chan struct{}),
	}
	live.sessions[session.key] = session
	live.mu.Unlock()
	type prepared struct {
		ticket playbackTicket
		err    error
	}
	result := make(chan prepared, 1)
	go func() {
		defer close(session.established)
		port, err := live.prepareReceiver(operationContext, session)
		if err != nil {
			result <- prepared{ticket: playbackTicket{PlaybackID: id}, err: err}
			return
		}
		live.mu.Lock()
		session.rtpPort = port
		if session.state != livePreparing || operationContext.Err() != nil {
			live.mu.Unlock()
			_, err := live.finishFailedStart(session, context.Canceled, false)
			result <- prepared{ticket: playbackTicket{PlaybackID: id}, err: err}
			return
		}
		value, err := newStreamToken()
		if err != nil {
			live.mu.Unlock()
			_, err = live.finishFailedStart(session, err, false)
			result <- prepared{ticket: playbackTicket{PlaybackID: id}, err: err}
			return
		}
		expiresAt := live.sip.now().Add(s.tokenTTL)
		s.tokens.mu.Lock()
		s.tokens.tokens[value] = streamToken{streamID: id, expiresAt: expiresAt}
		s.tokens.mu.Unlock()
		live.mu.Unlock()
		deadline, _ := operationContext.Deadline()
		host := net.JoinHostPort(s.media.server.mediaIP, strconv.Itoa(int(s.media.server.httpPort)))
		psURL := (&url.URL{Scheme: "http", Host: host, Path: "/play/gb28181/" + id + "/" + value + ".ps"}).String()
		result <- prepared{ticket: playbackTicket{PlaybackID: id, PSURL: psURL, ExpiresAt: expiresAt, Deadline: deadline}}
		timer := time.NewTimer(s.tokenTTL)
		defer timer.Stop()
		select {
		case <-session.claimed:
		case <-timer.C:
			_, err := live.finishFailedStart(session, fmt.Errorf("playback ticket expired without claim"), false)
			live.logger.Info("playback unclaimed receiver ended", "stream_id", id, "error", err)
			return
		case <-operationContext.Done():
			_, _ = live.finishFailedStart(session, operationContext.Err(), false)
			return
		}
		live.mu.Lock()
		device, online := live.sip.devices.getOnline(deviceID, live.sip.now())
		_, channelExists := live.sip.channels.get(deviceID, channelID)
		if session.state != livePreparing || operationContext.Err() != nil || !online || !channelExists {
			live.mu.Unlock()
			_, err := live.finishFailedStart(session, fmt.Errorf("playback target changed before INVITE"), false)
			live.logger.Warn("playback invite abandoned", "stream_id", id, "error", err)
			return
		}
		session.state = liveInviting
		live.mu.Unlock()
		body, err := buildGBSDP(liveSDPParameters{
			channelID: channelID, mediaIP: s.media.server.mediaIP, rtpPort: port, payloadType: livePayloadType, ssrc: ssrc,
		}, startTime, endTime, purpose, speed)
		if err != nil {
			_, err = live.finishFailedStart(session, err, false)
		} else {
			_, err = live.invite(operationContext, session, device, body)
		}
		if err != nil {
			live.logger.Warn("playback INVITE failed", "stream_id", id, "error", err)
		}
	}()
	select {
	case prepared := <-result:
		if ctx.Err() == nil {
			return prepared.ticket, prepared.err
		}
	case <-ctx.Done():
	}
	cleanupContext, cleanupCancel := s.media.timeoutContext()
	defer cleanupCancel()
	live.mu.Lock()
	_ = live.stopSessionLocked(cleanupContext, session)
	return playbackTicket{PlaybackID: id}, ctx.Err()
}

func (s *liveService) stopPlaybackID(ctx context.Context, id string) error {
	s.mu.Lock()
	for _, session := range s.sessions {
		if session.streamID == id && session.key.historyID != "" {
			return s.stopSessionLocked(ctx, session)
		}
	}
	s.mu.Unlock()
	return errLiveNotFound
}

func (s *infrastructureServer) handlePlaybackDelete(writer http.ResponseWriter, request *http.Request) {
	id := request.PathValue("playback_id")
	if !validUUIDv4(id) {
		writeHTTPError(writer, http.StatusBadRequest, "invalid_request")
		return
	}
	if err := s.live.stopPlaybackID(request.Context(), id); err != nil {
		if errors.Is(err, errLiveNotFound) {
			writeHTTPError(writer, http.StatusNotFound, "playback_not_found")
		} else {
			s.logger.Error("playback stop failed", "playback_id", id, "error", err)
			writeHTTPError(writer, http.StatusBadGateway, "playback_stop_failed")
		}
		return
	}
	writer.WriteHeader(http.StatusNoContent)
}

func (s *infrastructureServer) handlePlaybackControl(writer http.ResponseWriter, request *http.Request) {
	id, action := request.PathValue("playback_id"), request.PathValue("action")
	if !validUUIDv4(id) || (action != "pause" && action != "resume") {
		writeHTTPError(writer, http.StatusBadRequest, "invalid_request")
		return
	}
	err := s.live.controlPlayback(request.Context(), id, action == "pause")
	switch {
	case err == nil:
		writer.WriteHeader(http.StatusNoContent)
	case errors.Is(err, errLiveNotFound):
		writeHTTPError(writer, http.StatusNotFound, "playback_not_found")
	case errors.Is(err, errPlaybackNotStreaming):
		writeHTTPError(writer, http.StatusConflict, "playback_not_streaming")
	case errors.Is(err, errPlaybackControlBusy):
		writeHTTPError(writer, http.StatusConflict, "playback_control_busy")
	case errors.Is(err, errPlaybackRejected):
		s.logger.Warn("playback control rejected", "playback_id", id, "action", action, "error", err)
		writeHTTPError(writer, http.StatusBadGateway, "device_rejected")
	default:
		// 超时或传输失败时设备可能已执行，不自动发送相反指令。
		s.logger.Warn("playback control result unknown", "playback_id", id, "action", action, "error", err)
		status := http.StatusBadGateway
		if errors.Is(err, context.DeadlineExceeded) || errors.Is(err, sip.ErrTransactionTimeout) {
			status = http.StatusGatewayTimeout
		}
		writeHTTPError(writer, status, "playback_control_failed")
	}
}

// controlPlayback 在原对话内发送一次 GB/T 28181-2016 附录 B 的 MANSRTSP INFO；暂停不改变 owner 期限。
func (s *liveService) controlPlayback(ctx context.Context, id string, pause bool) error {
	s.mu.Lock()
	var session *liveSession
	for _, candidate := range s.sessions {
		if candidate.streamID == id && candidate.key.historyID != "" {
			session = candidate
		}
	}
	s.mu.Unlock()
	if session == nil {
		return errLiveNotFound
	}
	if !session.dialog.TryLock() {
		return errPlaybackControlBusy
	}
	defer session.dialog.Unlock()
	// 取得对话门后复查：停止可能已先发出 BYE。
	s.mu.Lock()
	streaming := s.sessions[session.key] == session && session.state == liveStreaming && session.ctx.Err() == nil
	s.mu.Unlock()
	if !streaming {
		return errPlaybackNotStreaming
	}
	controlContext, cancel := context.WithTimeout(ctx, playbackControlTimeout)
	defer cancel()
	defer context.AfterFunc(session.ctx, cancel)()
	request := session.nextDialogRequest(sip.INFO)
	sequence := request.CSeq().SeqNo
	body := fmt.Sprintf("PLAY RTSP/1.0\r\nCSeq: %d\r\nRange: npt=now-\r\n\r\n", sequence)
	if pause {
		body = fmt.Sprintf("PAUSE RTSP/1.0\r\nCSeq: %d\r\nPauseTime: now\r\n\r\n", sequence)
	}
	request.AppendHeader(sip.NewHeader("Content-Type", "Application/MANSRTSP"))
	request.SetBody([]byte(body))
	response, err := s.sip.client.Do(controlContext, request)
	if err != nil {
		return err
	}
	if response.StatusCode != 200 {
		return fmt.Errorf("%w: %w", errPlaybackRejected, sipgo.ErrDialogResponse{Res: response})
	}
	// 附录 J 示例的 200 可无内层响应；有内层响应时须为同一 CSeq 的 200。
	if inner := strings.TrimSpace(string(response.Body())); inner != "" {
		lines := strings.Split(strings.ReplaceAll(inner, "\r\n", "\n"), "\n")
		status := strings.Fields(lines[0])
		matched := false
		for _, line := range lines[1:] {
			name, value, found := strings.Cut(line, ":")
			if found && strings.EqualFold(strings.TrimSpace(name), "CSeq") {
				matched = strings.TrimSpace(value) == strconv.FormatUint(uint64(sequence), 10)
			}
		}
		if len(status) < 2 || status[0] != "RTSP/1.0" || status[1] != "200" || !matched {
			return fmt.Errorf("%w: MANSRTSP response %q", errPlaybackRejected, lines[0])
		}
	}
	return nil
}

func (s *liveService) handlePlaybackEnd(request *sip.Request, transaction sip.ServerTransaction, channelID string) {
	session := s.dialogSession(request)
	if session == nil || session.key.historyID == "" || session.key.channelID != channelID {
		_ = transaction.Respond(sip.NewResponseFromRequest(request, 481, "Call/Transaction Does Not Exist", nil))
		return
	}
	if err := transaction.Respond(sip.NewResponseFromRequest(request, 200, "OK", nil)); err != nil {
		s.logger.Warn("playback MediaStatus response failed", "stream_id", session.streamID, "error", err)
	}
	s.finishPlayback(session, true)
}

func (s *liveService) finishPlayback(session *liveSession, sendBye bool) {
	// ACK 后设备可立即通知结束，而 INVITE owner 还在校验 SDP 或更新接收 SSRC。
	<-session.established
	s.mu.Lock()
	if s.sessions[session.key] != session || session.state != liveStreaming {
		s.mu.Unlock()
		return
	}
	session.state = liveStopping
	s.tokens.revokeStream(session.streamID)
	s.mu.Unlock()
	go func() {
		if sendBye {
			s.sendBye(session)
		}
		// SIP 结束并不证明 TCP 媒体尾部已排空；给现有接收器一个有界收尾窗口。
		timer := time.NewTimer(playbackDrainTimeout)
		defer timer.Stop()
		select {
		case <-session.ctx.Done():
		case <-timer.C:
			s.logger.Warn("playback drain deadline reached; transfer may be incomplete", "stream_id", session.streamID)
		}
		if err := s.cleanup(session, false); err != nil {
			s.logger.Warn("playback end cleanup failed", "stream_id", session.streamID, "error", err)
		}
	}()
}
