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
)

var (
	errLiveExists         = errors.New("live session already exists")
	errLiveNotFound       = errors.New("live session does not exist")
	errDeviceOffline      = errors.New("device is offline")
	errChannelUnavailable = errors.New("channel is unavailable")
	errNoMediaServer      = errors.New("no online media server")
)

type liveState string

const (
	livePreparing liveState = "preparing"
	liveInviting  liveState = "inviting"
	liveStreaming liveState = "streaming"
	liveStopping  liveState = "stopping"
)

type liveKey struct {
	deviceID  string
	channelID string
}

type liveSession struct {
	key         liveKey
	streamName  string
	server      mediaServerInstance
	endpoint    mediaEndpoint
	ssrc        uint32
	state       liveState
	dialog      *sipgo.DialogClientSession
	callID      string
	cancel      context.CancelFunc
	established chan struct{}
	done        chan struct{}
	deleteMedia bool
}

type liveView struct {
	streamName string
	state      liveState
	ssrc       uint32
	rtpPort    uint16
}

type liveService struct {
	mu             sync.Mutex
	sessions       map[liveKey]*liveSession
	sip            *sipServer
	mediaServers   *mediaServerRegistry
	media          *mediaServerHTTPClient
	ssrcs          *ssrcAllocator
	logger         *slog.Logger
	inviteTimeout  time.Duration
	byeTimeout     time.Duration
	cleanupTimeout time.Duration
}

func newLiveService(sipServer *sipServer,
	mediaServers *mediaServerRegistry,
	media *mediaServerHTTPClient,
	ssrcs *ssrcAllocator,
	logger *slog.Logger) *liveService {
	service := &liveService{
		sessions:       make(map[liveKey]*liveSession),
		sip:            sipServer,
		mediaServers:   mediaServers,
		media:          media,
		ssrcs:          ssrcs,
		logger:         logger,
		inviteTimeout:  10 * time.Second,
		byeTimeout:     3 * time.Second,
		cleanupTimeout: 3 * time.Second,
	}
	sipServer.server.OnBye(service.handleRemoteBye)
	return service
}

func (s *liveService) startLive(ctx context.Context, deviceID, channelID string) (liveView, error) {
	device, ok := s.sip.devices.getOnline(deviceID, s.sip.now())
	if !ok {
		return liveView{}, errDeviceOffline
	}
	channel, ok := s.sip.channels.get(deviceID, channelID)
	if !ok || channel.status != "ON" {
		return liveView{}, errChannelUnavailable
	}
	server, ok := s.mediaServers.selectOnline()
	if !ok {
		return liveView{}, errNoMediaServer
	}
	ssrc, err := s.ssrcs.acquire()
	if err != nil {
		return liveView{}, err
	}
	key := liveKey{deviceID: deviceID, channelID: channelID}
	operationContext, cancel := context.WithCancel(ctx)
	session := &liveSession{
		key: key, streamName: "gb/" + deviceID + "/" + channelID, server: server, ssrc: ssrc,
		state: livePreparing, cancel: cancel, established: make(chan struct{}), done: make(chan struct{}), deleteMedia: true,
	}
	s.mu.Lock()
	if _, exists := s.sessions[key]; exists {
		s.mu.Unlock()
		cancel()
		s.ssrcs.release(ssrc)
		return liveView{}, errLiveExists
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
		return liveView{}, errChannelUnavailable
	}
	if !s.mediaServers.isOnline(server) {
		s.remove(session)
		return liveView{}, errNoMediaServer
	}

	endpoint, err := s.media.createUDPInput(operationContext, server, mediaInputRequest{
		streamName: session.streamName, payloadType: 96, ssrc: ssrc,
	})
	if err != nil {
		var rejection *mediaServerHTTPRejection
		if !errors.As(err, &rejection) && s.shouldDeleteMedia(session) {
			cleanupContext, cleanupCancel := context.WithTimeout(context.Background(), s.cleanupTimeout)
			_ = s.media.deleteInput(cleanupContext, server, session.streamName)
			cleanupCancel()
		}
		s.remove(session)
		return liveView{}, err
	}
	s.mu.Lock()
	session.endpoint = endpoint
	if session.state != livePreparing {
		s.mu.Unlock()
		s.cleanup(session, false, true)
		return liveView{}, context.Canceled
	}
	session.state = liveInviting
	s.mu.Unlock()

	sdpBody, err := buildLiveUDPSDP(liveSDPParameters{
		channelID: channelID, mediaIP: endpoint.address, rtpPort: endpoint.rtpPort, payloadType: endpoint.payloadType, ssrc: endpoint.ssrc,
	})
	if err != nil {
		s.cleanup(session, false, true)
		return liveView{}, err
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
		s.cleanup(session, false, true)
		return liveView{}, err
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
		s.cleanup(session, false, true)
		return liveView{}, err
	}
	contentType := dialog.InviteResponse.ContentType()
	mediaType := ""
	if contentType != nil {
		mediaType, _, _ = mime.ParseMediaType(contentType.Value())
	}
	if !strings.EqualFold(mediaType, "application/sdp") || validateLiveUDPAnswer(dialog.InviteResponse.Body(), endpoint.payloadType, endpoint.ssrc) != nil {
		ackContext, ackCancel := context.WithTimeout(context.Background(), s.byeTimeout)
		_ = dialog.Ack(ackContext)
		_ = dialog.Bye(ackContext)
		ackCancel()
		s.cleanup(session, false, true)
		return liveView{}, fmt.Errorf("invalid INVITE answer SDP")
	}
	ackContext, ackCancel := context.WithTimeout(operationContext, s.byeTimeout)
	err = dialog.Ack(ackContext)
	ackCancel()
	if err != nil {
		_ = dialog.Close()
		s.cleanup(session, false, true)
		return liveView{}, err
	}
	s.mu.Lock()
	if session.state != liveInviting {
		s.mu.Unlock()
		s.cleanup(session, true, true)
		return liveView{}, context.Canceled
	}
	session.state = liveStreaming
	view := liveView{streamName: session.streamName, state: session.state, ssrc: session.ssrc, rtpPort: session.endpoint.rtpPort}
	s.mu.Unlock()
	return view, nil
}

func (s *liveService) stopLive(ctx context.Context, deviceID, channelID string) error {
	key := liveKey{deviceID: deviceID, channelID: channelID}
	s.mu.Lock()
	session, ok := s.sessions[key]
	if !ok {
		s.mu.Unlock()
		return errLiveNotFound
	}
	s.mu.Unlock()
	return s.stopSession(ctx, session, true)
}

func (s *liveService) stopSession(ctx context.Context, session *liveSession, deleteMedia bool) error {
	s.mu.Lock()
	current, ok := s.sessions[session.key]
	if !ok || current != session {
		s.mu.Unlock()
		return nil
	}
	if !deleteMedia {
		session.deleteMedia = false
	}
	if session.state == livePreparing || session.state == liveInviting {
		session.state = liveStopping
		session.cancel()
		established := session.established
		s.mu.Unlock()
		select {
		case <-established:
			return nil
		case <-ctx.Done():
			return ctx.Err()
		}
	}
	if session.state == liveStopping {
		done := session.done
		s.mu.Unlock()
		select {
		case <-done:
			return nil
		case <-ctx.Done():
			return ctx.Err()
		}
	}
	session.state = liveStopping
	s.mu.Unlock()
	return s.cleanup(session, true, true)
}

func (s *liveService) deviceOffline(ctx context.Context, deviceID string) {
	s.sip.channels.removeDevice(deviceID)
	s.stopMatching(ctx, func(session *liveSession) bool { return session.key.deviceID == deviceID }, true)
}

func (s *liveService) mediaServerOffline(ctx context.Context, server mediaServerInstance) {
	s.stopMatching(ctx, func(session *liveSession) bool {
		return session.server.serverID == server.serverID && session.server.instanceID == server.instanceID
	}, false)
}

func (s *liveService) shutdown(ctx context.Context) {
	s.stopMatching(ctx, func(*liveSession) bool { return true }, true)
}

func (s *liveService) stopMatching(ctx context.Context, matches func(*liveSession) bool, deleteMedia bool) {
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
			if err := s.stopSession(ctx, session, deleteMedia); err != nil {
				s.logger.Warn("live cleanup failed", "stream_name", session.streamName, "error", err)
			}
		}()
	}
	wait.Wait()
}

func (s *liveService) cleanup(session *liveSession, sendBye, deleteMedia bool) error {
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
	if deleteMedia && s.shouldDeleteMedia(session) && session.endpoint.rtpPort != 0 {
		cleanupContext, cancel := context.WithTimeout(context.Background(), s.cleanupTimeout)
		if err := s.media.deleteInput(cleanupContext, session.server, session.streamName); err != nil {
			result = errors.Join(result, err)
		}
		cancel()
	}
	s.remove(session)
	return result
}

func (s *liveService) shouldDeleteMedia(session *liveSession) bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	return session.deleteMedia
}

func (s *liveService) remove(session *liveSession) {
	s.mu.Lock()
	if current, ok := s.sessions[session.key]; ok && current == session {
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
	s.mu.Unlock()
	if err := session.dialog.ReadBye(request, transaction); err != nil {
		s.logger.Warn("remote BYE failed", "device_id", session.key.deviceID, "channel_id", session.key.channelID, "error", err)
	}
	go func() {
		if err := s.cleanup(session, false, true); err != nil {
			s.logger.Warn("remote BYE cleanup failed", "stream_name", session.streamName, "error", err)
		}
	}()
}

func (s *liveService) len() int {
	s.mu.Lock()
	defer s.mu.Unlock()
	return len(s.sessions)
}
