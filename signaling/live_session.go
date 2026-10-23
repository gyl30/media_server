package main

import (
	"context"
	"errors"
	"fmt"
	"log/slog"
	"mime"
	"net"
	"strings"
	"sync"
	"time"

	"github.com/emiago/sipgo"
	"github.com/emiago/sipgo/sip"
	"github.com/google/uuid"
)

var (
	errLiveStopping    = errors.New("live session is stopping")
	errLiveNotFound    = errors.New("live session does not exist")
	errLiveChanged     = errors.New("live session generation changed")
	errDeviceOffline   = errors.New("device is offline")
	errDeviceStopping  = errors.New("device is stopping")
	errChannelNotFound = errors.New("channel not found")
	errChannelOffline  = errors.New("channel is offline")
)

type liveState string

const livePayloadType uint8 = 96

const (
	livePreparing      liveState = "preparing"
	liveInviting       liveState = "inviting"
	liveStreaming      liveState = "streaming"
	liveStopping       liveState = "stopping"
	liveCleanupPending liveState = "cleanup_pending"
)

type liveKey struct {
	deviceID  string
	channelID string
	historyID string
}

type liveSession struct {
	key              liveKey
	streamID         string
	rtpPort          uint16
	ssrc             uint32
	state            liveState
	invite           *sip.Request
	answer           *sip.Response
	ctx              context.Context
	cancel           context.CancelFunc
	claimed          chan struct{}
	established      chan struct{}
	done             chan struct{}
	cleanupErr       error
	playbackDuration int64
	dialog           sync.Mutex
	cseq             uint32
}

type liveView struct {
	streamID string
	state    liveState
	ssrc     uint32
	rtpPort  uint16
}

type liveService struct {
	mu              sync.Mutex
	sessions        map[liveKey]*liveSession
	tokens          *streamTokens
	stoppingDevices map[string]struct{}
	ptzOwners       map[liveKey]context.CancelFunc
	broadcasts      map[string]*broadcastSession
	sip             *sipServer
	media           *mediaServerHTTPClient
	ssrcs           *ssrcAllocator
	logger          *slog.Logger
	inviteTimeout   time.Duration
	byeTimeout      time.Duration
}

func newLiveService(
	sipServer *sipServer,
	media *mediaServerHTTPClient,
	ssrcs *ssrcAllocator,
	inviteTimeout time.Duration,
	byeTimeout time.Duration,
	logger *slog.Logger,
) *liveService {
	service := &liveService{
		sessions:        make(map[liveKey]*liveSession),
		stoppingDevices: make(map[string]struct{}),
		ptzOwners:       make(map[liveKey]context.CancelFunc),
		broadcasts:      make(map[string]*broadcastSession),
		sip:             sipServer,
		media:           media,
		ssrcs:           ssrcs,
		logger:          logger,
		inviteTimeout:   inviteTimeout,
		byeTimeout:      byeTimeout,
	}
	sipServer.server.OnBye(sipServer.handleBye)
	sipServer.server.OnInvite(sipServer.handleBroadcastInvite)
	sipServer.server.OnAck(sipServer.handleBroadcastAck)
	sipServer.live = service
	return service
}

func (s *liveService) startLive(ctx context.Context, deviceID, channelID string) (liveView, error) {
	s.mu.Lock()
	if _, err := s.sip.deviceStore.get(ctx, deviceID); err != nil {
		s.mu.Unlock()
		return liveView{}, err
	}
	if _, stopping := s.stoppingDevices[deviceID]; stopping {
		s.mu.Unlock()
		return liveView{}, errDeviceStopping
	}
	device, ok := s.sip.devices.getOnline(deviceID, s.sip.now())
	if !ok {
		s.mu.Unlock()
		return liveView{}, errDeviceOffline
	}
	channel, ok := s.sip.channels.get(deviceID, channelID)
	if !ok {
		s.mu.Unlock()
		return liveView{}, errChannelNotFound
	}
	if channel.status != "ON" {
		s.mu.Unlock()
		return liveView{}, errChannelOffline
	}
	key := liveKey{deviceID: deviceID, channelID: channelID}
	if existing, exists := s.sessions[key]; exists {
		if existing.state == liveStopping || existing.state == liveCleanupPending {
			s.mu.Unlock()
			return liveView{}, errLiveStopping
		}
		established := existing.established
		s.mu.Unlock()
		select {
		case <-established:
			s.mu.Lock()
			defer s.mu.Unlock()
			if s.sessions[key] != existing || existing.state != liveStreaming {
				return liveView{}, errLiveChanged
			}
			return makeLiveView(existing), nil
		case <-ctx.Done():
			return liveView{}, ctx.Err()
		}
	}
	ssrc, err := s.ssrcs.acquire(liveReceiver)
	if err != nil {
		s.mu.Unlock()
		return liveView{}, err
	}
	operationContext, cancel := context.WithCancel(ctx)
	session := &liveSession{
		key: key, streamID: uuid.NewString(), ssrc: ssrc,
		state: livePreparing, ctx: operationContext, cancel: cancel, established: make(chan struct{}), done: make(chan struct{}),
	}
	s.sessions[key] = session
	s.mu.Unlock()
	defer close(session.established)
	device, ok = s.sip.devices.getOnline(deviceID, s.sip.now())
	channel, channelOnline := s.sip.channels.get(deviceID, channelID)
	if !ok {
		s.remove(session)
		return liveView{}, errDeviceOffline
	}
	if !channelOnline || channel.status != "ON" {
		s.remove(session)
		return liveView{}, errChannelOffline
	}
	rtpPort, err := s.prepareReceiver(operationContext, session)
	if err != nil {
		s.mu.Lock()
		view := makeLiveView(session)
		s.mu.Unlock()
		return view, err
	}
	s.mu.Lock()
	session.rtpPort = rtpPort
	if session.state != livePreparing {
		s.mu.Unlock()
		return s.finishFailedStart(session, context.Canceled, false)
	}
	session.state = liveInviting
	s.mu.Unlock()

	sdpBody, err := buildGBSDP(liveSDPParameters{
		channelID: channelID, mediaIP: s.media.server.mediaIP, rtpPort: session.rtpPort, payloadType: livePayloadType, ssrc: ssrc,
	}, 0, 0, sessionPlay, 0)
	if err != nil {
		return s.finishFailedStart(session, err, false)
	}
	return s.invite(operationContext, session, device, sdpBody)
}

func (s *liveService) prepareReceiver(ctx context.Context, session *liveSession) (uint16, error) {
	kind := liveReceiver
	if session.key.historyID != "" {
		kind = playbackReceiver
	}
	rtpPort, err := s.media.createReceiver(ctx, gb28181ReceiverRequest{
		streamID: session.streamID, payloadType: livePayloadType, ssrc: session.ssrc,
	}, kind)
	if err == nil {
		return rtpPort, nil
	}
	_, rejected := errors.AsType[*mediaServerHTTPRejection](err)
	networkError, networkFailure := errors.AsType[*net.OpError](err)
	ambiguousCreate := !rejected && !(networkFailure && networkError.Op == "dial")
	cleanupConfirmed := !ambiguousCreate
	if ambiguousCreate {
		cleanupContext, cleanupCancel := s.media.timeoutContext()
		cleanupErr := s.media.deleteReceiver(cleanupContext, session.streamID)
		cleanupCancel()
		cleanupConfirmed = cleanupErr == nil || isMediaServerNotFound(cleanupErr)
		if !cleanupConfirmed {
			s.logger.Warn("live create compensation failed", "stream_id", session.streamID, "error", cleanupErr)
		}
	}
	s.mu.Lock()
	if s.sessions[session.key] == session && !cleanupConfirmed {
		markCleanupPendingLocked(session, err)
	}
	s.mu.Unlock()
	if cleanupConfirmed {
		s.remove(session)
	}
	return 0, err
}

func (s *liveService) invite(operationContext context.Context, session *liveSession, device registeredDevice, sdpBody []byte) (liveView, error) {
	channelID, ssrc := session.key.channelID, session.ssrc
	recipient := *device.contact.Clone()
	recipient.User = channelID
	request := sip.NewRequest(sip.INVITE, recipient)
	fromParams := sip.NewParams()
	fromParams.Add("tag", sip.GenerateTagN(16))
	request.AppendHeader(&sip.FromHeader{
		Address: sip.Uri{Scheme: "sip", User: s.sip.cfg.sipID, Host: s.sip.cfg.sipDomain}, Params: fromParams,
	})
	request.AppendHeader(&sip.ToHeader{Address: recipient})
	request.AppendHeader(&sip.ContactHeader{Address: sip.Uri{
		Scheme: "sip", User: s.sip.cfg.sipID, Host: s.sip.advertiseHost, Port: s.sip.advertisePort,
	}})
	request.AppendHeader(sip.NewHeader("Subject", fmt.Sprintf("%s:%010d,%s:0", channelID, ssrc, s.sip.cfg.sipID)))
	request.AppendHeader(sip.NewHeader("Content-Type", "application/sdp"))
	request.AppendHeader(sip.NewHeader("Allow", "INVITE, ACK, INFO, CANCEL, BYE, OPTIONS, MESSAGE"))
	request.SetBody(sdpBody)
	request.SetTransport("UDP")
	request.SetDestination(device.remoteEndpoint)
	inviteContext, inviteCancel := context.WithTimeout(operationContext, s.inviteTimeout)
	defer inviteCancel()
	client := s.sip.client
	if err := sipgo.ClientRequestBuild(client, request); err != nil {
		return s.finishFailedStart(session, err, false)
	}
	transaction, err := s.sip.ua.TransactionLayer().NewClientTransaction(inviteContext, request)
	if err != nil {
		return s.finishFailedStart(session, err, false)
	}
	invite := request.Clone()
	logger := s.logger
	transaction.OnRetransmission(func(answer *sip.Response) {
		if answer.StatusCode < 200 || answer.StatusCode >= 300 {
			return
		}
		if _, err := sip.DialogIDFromResponse(answer); err != nil {
			return
		}
		if err := client.WriteRequest(newLiveDialogRequest(invite, answer, sip.ACK)); err != nil {
			logger.Warn("live ACK retransmission failed", "error", err)
		}
	})
	var answer *sip.Response
	var cancelTx sip.ClientTransaction
	defer func() {
		if answer == nil {
			transaction.Terminate()
		}
		if cancelTx != nil {
			cancelTx.Terminate()
		}
	}()
	s.mu.Lock()
	session.invite = invite
	s.mu.Unlock()
	if err := transaction.Init(); err != nil {
		return s.finishFailedStart(session, err, false)
	}
	cancelSignal := inviteContext.Done()
	var compensation <-chan time.Time
	var cancelResponses <-chan *sip.Response
	var cancelDone <-chan struct{}
	var cancelRequest *sip.Request
	provisional := false
	for answer == nil {
		if cancelSignal == nil && provisional && cancelRequest == nil {
			cancelRequest = sip.NewRequest(sip.CANCEL, *invite.Recipient.Clone())
			cancelRequest.AppendHeader(sip.HeaderClone(invite.Via()))
			cancelRequest.AppendHeader(sip.HeaderClone(invite.From()))
			cancelRequest.AppendHeader(sip.HeaderClone(invite.To()))
			cancelRequest.AppendHeader(sip.HeaderClone(invite.CallID()))
			cseq := *invite.CSeq()
			cseq.MethodName = sip.CANCEL
			cancelRequest.AppendHeader(&cseq)
			sip.CopyHeaders("Route", invite, cancelRequest)
			cancelRequest.SetDestination(invite.Destination())
			cancelRequest.SetTransport(invite.Transport())
			cancelRequest.SetSource(invite.Source())
			cancelRequest.Laddr = invite.Laddr
			cancelContext, cancel := context.WithTimeout(context.Background(), s.byeTimeout)
			cancelTx, err = client.TransactionRequest(cancelContext, cancelRequest)
			cancel()
			if err != nil {
				s.logger.Warn("live CANCEL failed", "stream_id", session.streamID, "error", err)
			} else {
				cancelResponses, cancelDone = cancelTx.Responses(), cancelTx.Done()
			}
		}
		select {
		case response := <-transaction.Responses():
			if response.IsProvisional() {
				provisional = true
				continue
			}
			answer = response
		case <-transaction.Done():
			cause := transaction.Err()
			if cause == nil {
				cause = fmt.Errorf("INVITE transaction ended without a final response")
			}
			return s.finishFailedStart(session, cause, false)
		case <-cancelSignal:
			cancelSignal = nil
			timer := time.NewTimer(s.byeTimeout)
			defer timer.Stop()
			compensation = timer.C
		case response := <-cancelResponses:
			if !response.IsProvisional() {
				cancelTx.Terminate()
				cancelResponses, cancelDone = nil, nil
			}
		case <-cancelDone:
			cancelResponses, cancelDone = nil, nil
		case <-compensation:
			return s.finishFailedStart(session, inviteContext.Err(), false)
		}
	}
	if answer.StatusCode < 200 || answer.StatusCode >= 300 {
		return s.finishFailedStart(session, sipgo.ErrDialogResponse{Res: answer}, false)
	}
	if _, err := sip.DialogIDFromResponse(answer); err != nil {
		return s.finishFailedStart(session, err, false)
	}
	s.mu.Lock()
	session.answer = answer.Clone()
	s.mu.Unlock()
	if err := client.WriteRequest(newLiveDialogRequest(invite, answer, sip.ACK)); err != nil {
		return s.finishFailedStart(session, err, true)
	}
	if err := inviteContext.Err(); err != nil {
		return s.finishFailedStart(session, err, true)
	}
	contentType := answer.ContentType()
	mediaType := ""
	if contentType != nil {
		mediaType, _, _ = mime.ParseMediaType(contentType.Value())
	}
	answerErr := fmt.Errorf("INVITE answer is not SDP")
	deviceSSRC := ssrc
	if strings.EqualFold(mediaType, "application/sdp") {
		deviceSSRC, answerErr = validateGBAnswer(answer.Body(), livePayloadType, ssrc, session.key.historyID != "")
	}
	if answerErr == nil && deviceSSRC != ssrc {
		s.logger.Info("device selected SSRC", "stream_id", session.streamID, "requested", ssrc, "device", deviceSSRC)
		answerErr = s.media.updateReceiverSSRC(operationContext, session.streamID, deviceSSRC)
	}
	if answerErr != nil {
		return s.finishFailedStart(session, fmt.Errorf("invalid INVITE answer: %w", answerErr), true)
	}
	s.mu.Lock()
	if cause := operationContext.Err(); session.state != liveInviting || cause != nil {
		s.mu.Unlock()
		if cause == nil {
			cause = context.Canceled
		}
		return s.finishFailedStart(session, cause, true)
	}
	session.state = liveStreaming
	view := makeLiveView(session)
	s.mu.Unlock()
	return view, nil
}

func (s *liveService) finishFailedStart(session *liveSession, cause error, sendBye bool) (liveView, error) {
	cleanupErr := s.cleanup(session, sendBye)
	s.mu.Lock()
	view := makeLiveView(session)
	s.mu.Unlock()
	return view, errors.Join(cause, cleanupErr)
}

func (s *liveService) live(deviceID, channelID string) (liveView, bool) {
	s.mu.Lock()
	defer s.mu.Unlock()
	session, ok := s.sessions[liveKey{deviceID: deviceID, channelID: channelID}]
	if !ok {
		return liveView{}, false
	}
	return makeLiveView(session), true
}

func (s *liveService) stopLiveID(ctx context.Context, liveID string) error {
	s.mu.Lock()
	for _, session := range s.sessions {
		if session.streamID == liveID && session.key.historyID == "" {
			return s.stopSessionLocked(ctx, session)
		}
	}
	s.mu.Unlock()
	return errLiveNotFound
}

func (s *liveService) stopSessionLocked(ctx context.Context, session *liveSession) error {
	current, ok := s.sessions[session.key]
	if !ok || current != session {
		s.mu.Unlock()
		return nil
	}
	s.tokens.revokeStream(session.streamID)
	if session.state == liveCleanupPending {
		session.state = liveStopping
		session.cleanupErr = nil
		s.mu.Unlock()
		return s.cleanup(session, false)
	}
	if session.state == livePreparing || session.state == liveInviting {
		session.state = liveStopping
		session.cancel()
		established := session.established
		s.mu.Unlock()
		return s.waitForStop(ctx, session, established)
	}
	if session.state == liveStopping {
		session.cancel()
		done := session.done
		s.mu.Unlock()
		return s.waitForStop(ctx, session, done)
	}
	session.state = liveStopping
	session.cancel()
	s.mu.Unlock()
	return s.cleanup(session, true)
}

func (s *liveService) waitForStop(ctx context.Context, session *liveSession, wait <-chan struct{}) error {
	for {
		select {
		case <-wait:
			s.mu.Lock()
			current, exists := s.sessions[session.key]
			if !exists || current != session {
				s.mu.Unlock()
				return nil
			}
			if session.state == liveCleanupPending {
				err := session.cleanupErr
				s.mu.Unlock()
				return err
			}
			wait = session.done
			s.mu.Unlock()
		case <-ctx.Done():
			return ctx.Err()
		}
	}
}

func (s *liveService) shutdown(ctx context.Context) {
	s.mu.Lock()
	var broadcasts []*broadcastSession
	for _, session := range s.broadcasts {
		session.cancel(context.Canceled)
		broadcasts = append(broadcasts, session)
	}
	s.mu.Unlock()
	s.stopMatching(ctx, func(*liveSession) bool { return true })
	for _, session := range broadcasts {
		select {
		case <-session.done:
		case <-ctx.Done():
			return
		}
	}
}

func (s *liveService) stopMatching(ctx context.Context, matches func(*liveSession) bool) {
	s.mu.Lock()
	var sessions []*liveSession
	for _, session := range s.sessions {
		if matches(session) {
			sessions = append(sessions, session)
		}
	}
	s.mu.Unlock()
	var wait sync.WaitGroup
	for _, session := range sessions {
		wait.Add(1)
		go func() {
			defer wait.Done()
			s.mu.Lock()
			if err := s.stopSessionLocked(ctx, session); err != nil {
				s.logger.Warn("live cleanup failed", "stream_id", session.streamID, "error", err)
			}
		}()
	}
	wait.Wait()
}

func (s *liveService) cleanup(session *liveSession, sendBye bool) error {
	if sendBye && session.answer != nil {
		s.sendBye(session)
	}
	cleanupContext, cancel := s.media.timeoutContext()
	deleteErr := s.media.deleteReceiver(cleanupContext, session.streamID)
	cancel()
	mediaStopped := deleteErr == nil || isMediaServerNotFound(deleteErr)
	s.mu.Lock()
	if !mediaStopped {
		if current, ok := s.sessions[session.key]; ok && current == session {
			markCleanupPendingLocked(session, deleteErr)
		}
		s.mu.Unlock()
		return deleteErr
	}
	s.mu.Unlock()
	s.remove(session)
	return nil
}

func (s *liveService) sendBye(session *liveSession) {
	session.dialog.Lock()
	defer session.dialog.Unlock()
	byeContext, cancel := context.WithTimeout(context.Background(), s.byeTimeout)
	defer cancel()
	response, err := s.sip.client.Do(byeContext, session.nextDialogRequest(sip.BYE))
	if err == nil && response.StatusCode != 200 {
		err = sipgo.ErrDialogResponse{Res: response}
	}
	if err != nil {
		s.logger.Warn("live BYE failed", "stream_id", session.streamID, "error", err)
	}
}

func markCleanupPendingLocked(session *liveSession, err error) {
	session.state = liveCleanupPending
	session.cleanupErr = err
	close(session.done)
	session.done = make(chan struct{})
}

func (s *liveService) remove(session *liveSession) {
	s.mu.Lock()
	if current, ok := s.sessions[session.key]; ok && current == session {
		s.tokens.revokeStream(session.streamID)
		delete(s.sessions, session.key)
		session.cancel()
		s.ssrcs.release(session.ssrc)
		close(session.done)
	}
	s.mu.Unlock()
}

func (s *liveService) dialogSession(request *sip.Request) *liveSession {
	callID := request.CallID()
	dialogID, dialogError := sip.DialogIDFromRequestUAC(request)
	if callID == nil || dialogError != nil {
		return nil
	}
	s.mu.Lock()
	var session *liveSession
	for _, candidate := range s.sessions {
		if candidate.answer != nil &&
			(candidate.state == liveStreaming || (candidate.key.historyID != "" && candidate.state == liveInviting)) {
			if inviteCallID := candidate.invite.CallID(); inviteCallID == nil || inviteCallID.Value() != callID.Value() {
				continue
			}
			candidateID, err := sip.DialogIDFromResponse(candidate.answer)
			if err != nil || candidateID != dialogID {
				continue
			}
			session = candidate
			break
		}
	}
	s.mu.Unlock()
	if session == nil || !s.sip.devices.registeredSource(session.key.deviceID, request.Source(), s.sip.now()) {
		return nil
	}
	return session
}

func (s *liveService) handleRemoteBye(request *sip.Request, transaction sip.ServerTransaction) {
	session := s.dialogSession(request)
	if session == nil {
		_ = transaction.Respond(sip.NewResponseFromRequest(request, 481, "Call/Transaction Does Not Exist", nil))
		return
	}
	if session.key.historyID != "" {
		if err := transaction.Respond(sip.NewResponseFromRequest(request, 200, "OK", nil)); err != nil {
			s.logger.Warn("playback remote BYE response failed", "stream_id", session.streamID, "error", err)
		}
		s.finishPlayback(session, false)
		return
	}
	s.mu.Lock()
	current, exists := s.sessions[session.key]
	if !exists || current != session || session.state != liveStreaming {
		s.mu.Unlock()
		_ = transaction.Respond(sip.NewResponseFromRequest(request, 481, "Call/Transaction Does Not Exist", nil))
		return
	}
	session.state = liveStopping
	s.tokens.revokeStream(session.streamID)
	s.mu.Unlock()
	if err := transaction.Respond(sip.NewResponseFromRequest(request, 200, "OK", nil)); err != nil {
		s.logger.Warn("remote BYE failed", "device_id", session.key.deviceID, "channel_id", session.key.channelID, "error", err)
	}
	go func() {
		if err := s.cleanup(session, false); err != nil {
			s.logger.Warn("remote BYE cleanup failed", "stream_id", session.streamID, "error", err)
		}
	}()
}

func makeLiveView(session *liveSession) liveView {
	state := session.state
	if state == liveCleanupPending {
		state = liveStopping
	}
	return liveView{
		streamID: session.streamID, state: state,
		ssrc: session.ssrc, rtpPort: session.rtpPort,
	}
}
