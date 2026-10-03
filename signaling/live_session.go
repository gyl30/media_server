package main

import (
	"context"
	"errors"
	"fmt"
	"log/slog"
	"mime"
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
}

type liveSession struct {
	key         liveKey
	streamName  string
	streamID    string
	rtpPort     uint16
	ssrc        uint32
	state       liveState
	dialog      *sipgo.DialogClientSession
	callID      string
	cancel      context.CancelFunc
	established chan struct{}
	done        chan struct{}
	cleanupErr  error
	offers      sync.WaitGroup
}

type liveView struct {
	streamID   string
	streamName string
	state      liveState
	ssrc       uint32
	rtpPort    uint16
}

type liveService struct {
	mu            sync.Mutex
	sessions      map[liveKey]*liveSession
	tickets       map[string]playTicket
	sip           *sipServer
	media         *mediaServerHTTPClient
	ssrcs         *ssrcAllocator
	logger        *slog.Logger
	inviteTimeout time.Duration
	byeTimeout    time.Duration
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
		sessions:      make(map[liveKey]*liveSession),
		tickets:       make(map[string]playTicket),
		sip:           sipServer,
		media:         media,
		ssrcs:         ssrcs,
		logger:        logger,
		inviteTimeout: inviteTimeout,
		byeTimeout:    byeTimeout,
	}
	sipServer.server.OnBye(service.handleRemoteBye)
	return service
}

func (s *liveService) startLive(ctx context.Context, deviceID, channelID string) (liveView, error) {
	if _, err := s.sip.deviceStore.get(ctx, deviceID); err != nil {
		return liveView{}, err
	}
	device, ok := s.sip.devices.getOnline(deviceID, s.sip.now())
	if !ok {
		return liveView{}, errDeviceOffline
	}
	channel, ok := s.sip.channels.get(deviceID, channelID)
	if !ok {
		return liveView{}, errChannelNotFound
	}
	if channel.status != "ON" {
		return liveView{}, errChannelOffline
	}
	key := liveKey{deviceID: deviceID, channelID: channelID}
	s.mu.Lock()
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
	ssrc, err := s.ssrcs.acquire()
	if err != nil {
		s.mu.Unlock()
		return liveView{}, err
	}
	operationContext, cancel := context.WithCancel(ctx)
	session := &liveSession{
		key: key, streamID: uuid.NewString(), streamName: "gb/" + deviceID + "/" + channelID, ssrc: ssrc,
		state: livePreparing, cancel: cancel, established: make(chan struct{}), done: make(chan struct{}),
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
	rtpPort, err := s.media.createUDPReceiver(operationContext, gb28181ReceiverRequest{
		streamID: session.streamID, streamName: session.streamName, payloadType: livePayloadType, ssrc: ssrc,
	})
	if err != nil {
		var rejection *mediaServerHTTPRejection
		ambiguousCreate := !errors.As(err, &rejection)
		cleanupConfirmed := !ambiguousCreate
		if ambiguousCreate {
			cleanupContext, cleanupCancel := s.media.timeoutContext()
			cleanupErr := s.media.deleteReceiver(cleanupContext, session.streamID, session.streamName)
			cleanupCancel()
			cleanupConfirmed = cleanupErr == nil
			if !cleanupConfirmed {
				s.logger.Warn("live create compensation failed", "stream_name", session.streamName,
					"stream_id", session.streamID, "error", cleanupErr)
			}
		}
		s.mu.Lock()
		if current, ok := s.sessions[session.key]; ok && current == session && !cleanupConfirmed {
			markCleanupPendingLocked(session, err)
		}
		view := makeLiveView(session)
		s.mu.Unlock()
		if cleanupConfirmed {
			s.remove(session)
		}
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

	sdpBody, err := buildLiveUDPSDP(liveSDPParameters{
		channelID: channelID, mediaIP: s.media.server.mediaIP, rtpPort: rtpPort, payloadType: livePayloadType, ssrc: ssrc,
	})
	if err != nil {
		return s.finishFailedStart(session, err, false)
	}
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
	dialogUA := sipgo.DialogUA{
		Client: s.sip.client,
		ContactHDR: sip.ContactHeader{Address: sip.Uri{
			Scheme: "sip", User: s.sip.cfg.sipID, Host: s.sip.advertiseHost, Port: s.sip.advertisePort,
		}},
		RewriteContact: true,
	}
	dialog, err := dialogUA.WriteInvite(operationContext, request)
	if err != nil {
		return s.finishFailedStart(session, err, false)
	}
	s.mu.Lock()
	session.dialog = dialog
	if callID := dialog.InviteRequest.CallID(); callID != nil {
		session.callID = callID.Value()
	}
	s.mu.Unlock()
	inviteContext, inviteCancel := context.WithTimeout(operationContext, s.inviteTimeout)
	err = dialog.WaitAnswer(inviteContext, sipgo.AnswerOptions{})
	inviteCancel()
	if err != nil {
		_ = dialog.Close()
		return s.finishFailedStart(session, err, false)
	}
	contentType := dialog.InviteResponse.ContentType()
	mediaType := ""
	if contentType != nil {
		mediaType, _, _ = mime.ParseMediaType(contentType.Value())
	}
	if !strings.EqualFold(mediaType, "application/sdp") || validateLiveUDPAnswer(dialog.InviteResponse.Body(), livePayloadType, ssrc) != nil {
		ackContext, ackCancel := context.WithTimeout(context.Background(), s.byeTimeout)
		_ = dialog.Ack(ackContext)
		_ = dialog.Bye(ackContext)
		ackCancel()
		return s.finishFailedStart(session, fmt.Errorf("invalid INVITE answer SDP"), false)
	}
	ackContext, ackCancel := context.WithTimeout(operationContext, s.byeTimeout)
	err = dialog.Ack(ackContext)
	ackCancel()
	if err != nil {
		_ = dialog.Close()
		return s.finishFailedStart(session, err, false)
	}
	s.mu.Lock()
	if session.state != liveInviting {
		s.mu.Unlock()
		return s.finishFailedStart(session, context.Canceled, true)
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

func (s *liveService) stopLive(ctx context.Context, deviceID, channelID, expectedStreamID string) error {
	key := liveKey{deviceID: deviceID, channelID: channelID}
	s.mu.Lock()
	session, ok := s.sessions[key]
	if !ok {
		s.mu.Unlock()
		return errLiveNotFound
	}
	if expectedStreamID != "" && session.streamID != expectedStreamID {
		s.mu.Unlock()
		return errLiveChanged
	}
	return s.stopSessionLocked(ctx, session)
}

func (s *liveService) stopLiveID(ctx context.Context, liveID string) error {
	s.mu.Lock()
	for _, session := range s.sessions {
		if session.streamID == liveID {
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
	s.invalidateLiveTicketsLocked(session.streamID)
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
		done := session.done
		s.mu.Unlock()
		return s.waitForStop(ctx, session, done)
	}
	session.state = liveStopping
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

func (s *liveService) deviceOffline(ctx context.Context, deviceID string) {
	s.sip.channels.removeDevice(deviceID)
	s.stopMatching(ctx, func(session *liveSession) bool { return session.key.deviceID == deviceID })
}

func (s *liveService) shutdown(ctx context.Context) {
	s.stopMatching(ctx, func(*liveSession) bool { return true })
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
				s.logger.Warn("live cleanup failed", "stream_name", session.streamName, "error", err)
			}
		}()
	}
	wait.Wait()
}

func (s *liveService) cleanup(session *liveSession, sendBye bool) error {
	// Finish offers against this source before deleting it or allowing a new generation.
	session.offers.Wait()
	var result error
	if sendBye && session.dialog != nil {
		byeContext, cancel := context.WithTimeout(context.Background(), s.byeTimeout)
		if err := session.dialog.Bye(byeContext); err != nil {
			result = err
		}
		cancel()
	} else if session.dialog != nil {
		_ = session.dialog.Close()
	}
	cleanupContext, cancel := s.media.timeoutContext()
	deleteErr := s.media.deleteReceiver(cleanupContext, session.streamID, session.streamName)
	cancel()
	mediaStopped := deleteErr == nil || isMediaServerNotFound(deleteErr)
	if !mediaStopped {
		result = errors.Join(result, deleteErr)
	}
	s.mu.Lock()
	if !mediaStopped {
		if current, ok := s.sessions[session.key]; ok && current == session {
			markCleanupPendingLocked(session, result)
		}
		s.mu.Unlock()
		return result
	}
	s.mu.Unlock()
	s.remove(session)
	return result
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
		s.invalidateLiveTicketsLocked(session.streamID)
		delete(s.sessions, session.key)
		session.cancel()
		s.ssrcs.release(session.ssrc)
		close(session.done)
	}
	s.mu.Unlock()
}

func (s *liveService) handleRemoteBye(request *sip.Request, transaction sip.ServerTransaction) {
	callID := request.CallID()
	dialogID, dialogError := sip.DialogIDFromRequestUAC(request)
	if callID == nil || dialogError != nil {
		_ = transaction.Respond(sip.NewResponseFromRequest(request, 481, "Call/Transaction Does Not Exist", nil))
		return
	}
	s.mu.Lock()
	var session *liveSession
	for _, candidate := range s.sessions {
		if candidate.callID == callID.Value() && candidate.state == liveStreaming && candidate.dialog != nil && candidate.dialog.ID == dialogID {
			session = candidate
			break
		}
	}
	s.mu.Unlock()
	if session == nil || !s.sip.devices.registeredSource(session.key.deviceID, request.Source(), s.sip.now()) {
		_ = transaction.Respond(sip.NewResponseFromRequest(request, 481, "Call/Transaction Does Not Exist", nil))
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
	s.invalidateLiveTicketsLocked(session.streamID)
	s.mu.Unlock()
	if err := session.dialog.ReadBye(request, transaction); err != nil {
		s.logger.Warn("remote BYE failed", "device_id", session.key.deviceID, "channel_id", session.key.channelID, "error", err)
	}
	go func() {
		if err := s.cleanup(session, false); err != nil {
			s.logger.Warn("remote BYE cleanup failed", "stream_name", session.streamName, "error", err)
		}
	}()
}

func makeLiveView(session *liveSession) liveView {
	state := session.state
	if state == liveCleanupPending {
		state = liveStopping
	}
	return liveView{
		streamID: session.streamID, streamName: session.streamName, state: state,
		ssrc: session.ssrc, rtpPort: session.rtpPort,
	}
}
