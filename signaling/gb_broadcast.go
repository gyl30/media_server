package main

import (
	"context"
	"errors"
	"fmt"
	"mime"
	"net/netip"
	"strings"

	"github.com/emiago/sipgo"
	"github.com/emiago/sipgo/sip"
)

var (
	errBroadcastBusy      = errors.New("device broadcast is occupied")
	errBroadcastNotFound  = errors.New("broadcast does not exist")
	errBroadcastRejected  = errors.New("device rejected broadcast")
	errBroadcastRemoteBye = errors.New("device ended broadcast")
)

// 控制 owner 不持有媒体 socket；调用方须先绑定端口，并在整个媒体寿命内持有 ctx。
type broadcastEndpoint struct {
	rtp  netip.AddrPort
	rtcp netip.AddrPort
}

type broadcastPeer struct {
	endpoint broadcastEndpoint
	ssrc     uint32
}

type broadcastResult struct {
	sourceID string
	peer     broadcastPeer
	ctx      context.Context // 媒体 owner 必须监听 Done；远端 BYE/下线不会取消调用方的父 ctx。
	err      error
}

type broadcastDialog struct {
	session    *sipgo.DialogServerSession
	tx         sip.ServerTransaction
	peer       broadcastPeer
	acked      chan struct{}
	answerDone chan struct{}
	ackDone    chan struct{}
	answerErr  error
}

type broadcastSession struct {
	deviceID   string
	sourceID   string
	sn         int
	local      broadcastEndpoint
	ctx        context.Context
	cancel     context.CancelCauseFunc
	business   chan error
	invited    chan *broadcastDialog
	result     chan broadcastResult
	done       chan struct{}
	dialog     *broadcastDialog // live.mu 保护发布，answerDone 后才读取 InviteResponse。
	cleanupErr error
}

func (s *liveService) startBroadcast(ctx context.Context, deviceID string, local broadcastEndpoint) (broadcastResult, error) {
	if !local.rtp.IsValid() || !local.rtp.Addr().Is4() || local.rtp.Addr().IsUnspecified() || local.rtp.Addr().IsMulticast() ||
		local.rtp.Port() == 0 || local.rtcp.Addr() != local.rtp.Addr() || local.rtcp.Port() == 0 || local.rtcp.Port() == local.rtp.Port() {
		return broadcastResult{}, fmt.Errorf("invalid broadcast media endpoint")
	}
	s.mu.Lock()
	if err := ctx.Err(); err != nil {
		s.mu.Unlock()
		return broadcastResult{}, err
	}
	if _, err := s.sip.deviceStore.get(ctx, deviceID); err != nil {
		s.mu.Unlock()
		return broadcastResult{}, err
	}
	if _, stopping := s.stoppingDevices[deviceID]; stopping {
		s.mu.Unlock()
		return broadcastResult{}, errDeviceStopping
	}
	device, online := s.sip.devices.getOnline(deviceID, s.sip.now())
	if !online {
		s.mu.Unlock()
		return broadcastResult{}, errDeviceOffline
	}
	if s.broadcasts[deviceID] != nil {
		s.mu.Unlock()
		return broadcastResult{}, errBroadcastBusy
	}
	sourceID, err := s.sip.deviceStore.nextBroadcastID(ctx, s.sip.cfg.sipDomain)
	if err != nil {
		s.mu.Unlock()
		return broadcastResult{}, err
	}
	operation, cancel := context.WithCancelCause(ctx)
	session := &broadcastSession{
		deviceID: deviceID, sourceID: sourceID, sn: int(s.sip.messageSN.Add(1)), local: local,
		ctx: operation, cancel: cancel, business: make(chan error, 1), invited: make(chan *broadcastDialog, 1),
		result: make(chan broadcastResult, 1), done: make(chan struct{}),
	}
	s.broadcasts[deviceID] = session
	s.mu.Unlock()
	go s.runBroadcast(session, device)
	select {
	case result := <-session.result:
		if result.err == nil && ctx.Err() == nil && session.ctx.Err() == nil {
			return result, nil
		}
		<-session.done
		return broadcastResult{}, errors.Join(result.err, context.Cause(session.ctx), session.cleanupErr)
	case <-ctx.Done():
		<-session.done
		return broadcastResult{}, errors.Join(ctx.Err(), session.cleanupErr)
	}
}

func (s *liveService) runBroadcast(session *broadcastSession, device registeredDevice) {
	peer, err := s.prepareBroadcast(session, device)
	if err != nil {
		session.cancel(err)
	}
	session.result <- broadcastResult{sourceID: session.sourceID, peer: peer, ctx: session.ctx, err: err}
	<-session.ctx.Done()
	s.mu.Lock()
	dialog := session.dialog
	s.mu.Unlock()
	if dialog != nil {
		// Terminate 解除库内部应答等待；Close 自身不会取消事务。先 join，再读应答或 BYE。
		if dialog.session.LoadState() != sip.DialogStateConfirmed {
			dialog.tx.Terminate()
		}
		<-dialog.answerDone
		if context.Cause(session.ctx) != errBroadcastRemoteBye && dialog.session.InviteResponse != nil && dialog.session.InviteResponse.IsSuccess() {
			byeContext, cancel := context.WithTimeout(context.Background(), s.byeTimeout)
			session.cleanupErr = dialog.session.Bye(byeContext)
			cancel()
		}
		dialog.tx.Terminate()
		<-dialog.ackDone
		_ = dialog.session.Close()
	}
	s.mu.Lock()
	if s.broadcasts[session.deviceID] == session {
		delete(s.broadcasts, session.deviceID)
	}
	close(session.done)
	s.mu.Unlock()
}

func (s *liveService) prepareBroadcast(session *broadcastSession, device registeredDevice) (broadcastPeer, error) {
	ctx, cancel := context.WithTimeout(session.ctx, s.inviteTimeout)
	defer cancel()
	body := []byte(fmt.Sprintf("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n<Notify><CmdType>Broadcast</CmdType><SN>%d</SN><SourceID>%s</SourceID><TargetID>%s</TargetID></Notify>\r\n", session.sn, session.sourceID, session.deviceID))
	if err := s.sip.sendMANSCDP(ctx, device, body); err != nil {
		return broadcastPeer{}, err
	}
	business, invited := session.business, session.invited
	var acked, answered <-chan struct{}
	var dialog *broadcastDialog
	for business != nil || invited != nil || acked != nil || answered != nil {
		select {
		case err := <-business:
			if err != nil {
				return broadcastPeer{}, err
			}
			business = nil
		case dialog = <-invited:
			invited = nil
			acked, answered = dialog.acked, dialog.answerDone
		case <-acked:
			acked = nil
		case <-answered:
			if dialog.answerErr != nil {
				return broadcastPeer{}, dialog.answerErr
			}
			answered = nil
		case <-ctx.Done():
			return broadcastPeer{}, ctx.Err()
		}
	}
	return dialog.peer, nil
}

func (s *liveService) stopBroadcast(ctx context.Context, sourceID string) error {
	s.mu.Lock()
	var session *broadcastSession
	for _, candidate := range s.broadcasts {
		if candidate.sourceID == sourceID {
			session = candidate
			break
		}
	}
	if session != nil {
		session.cancel(context.Canceled)
	}
	s.mu.Unlock()
	if session == nil {
		return errBroadcastNotFound
	}
	select {
	case <-session.done:
		return session.cleanupErr
	case <-ctx.Done():
		return ctx.Err()
	}
}

func (s *sipServer) handleBroadcastResponse(req *sip.Request, tx sip.ServerTransaction, response manscdpEnvelope) {
	s.live.mu.Lock()
	session := s.live.broadcasts[response.DeviceID]
	from := req.From()
	if session == nil || session.ctx.Err() != nil || response.XMLName.Local != "Response" || response.CmdType != "Broadcast" ||
		response.SN != session.sn || from == nil || from.Address.User != session.deviceID ||
		!s.devices.registeredSource(session.deviceID, req.Source(), s.now()) {
		s.live.mu.Unlock()
		s.respond(tx, sip.NewResponseFromRequest(req, 403, "Forbidden", nil))
		return
	}
	var err error
	if response.Result != "OK" {
		err = errBroadcastRejected
		session.cancel(err)
	}
	select {
	case session.business <- err:
	default:
	}
	s.live.mu.Unlock()
	s.respond(tx, sip.NewResponseFromRequest(req, 200, "OK", nil))
}

func (s *sipServer) handleBroadcastInvite(req *sip.Request, tx sip.ServerTransaction) {
	from, to, ct, subject := req.From(), req.To(), req.ContentType(), req.GetHeader("Subject")
	mediaType := ""
	if ct != nil {
		mediaType, _, _ = mime.ParseMediaType(ct.Value())
	}
	if from == nil || to == nil || req.Recipient.Scheme != "sip" || req.Recipient.User != to.Address.User ||
		!strings.EqualFold(mediaType, "application/sdp") || len(req.Body()) > 64*1024 || subject == nil {
		s.respond(tx, sip.NewResponseFromRequest(req, 488, "Not Acceptable Here", nil))
		return
	}
	s.live.mu.Lock()
	session := s.live.broadcasts[from.Address.User]
	parts := strings.Split(subject.Value(), ",")
	if session == nil || session.ctx.Err() != nil || session.sourceID != to.Address.User ||
		len(parts) != 2 || !strings.HasPrefix(strings.TrimSpace(parts[0]), session.sourceID+":") ||
		!strings.HasPrefix(strings.TrimSpace(parts[1]), session.deviceID+":") ||
		!s.devices.registeredSource(session.deviceID, req.Source(), s.now()) {
		s.live.mu.Unlock()
		s.respond(tx, sip.NewResponseFromRequest(req, 403, "Forbidden", nil))
		return
	}
	if session.dialog != nil {
		s.live.mu.Unlock()
		s.respond(tx, sip.NewResponseFromRequest(req, 486, "Busy Here", nil))
		return
	}
	peer, err := parseBroadcastOffer(req.Body(), req.Source())
	if err != nil {
		s.live.mu.Unlock()
		s.respond(tx, sip.NewResponseFromRequest(req, 488, "Not Acceptable Here", nil))
		return
	}
	ua := sipgo.DialogUA{Client: s.client, RewriteContact: true, ContactHDR: sip.ContactHeader{Address: sip.Uri{
		Scheme: "sip", User: session.sourceID, Host: s.advertiseHost, Port: s.advertisePort,
	}}}
	dialog, err := ua.ReadInvite(req, tx)
	if err != nil {
		s.live.mu.Unlock()
		s.respond(tx, sip.NewResponseFromRequest(req, 400, "Bad Request", nil))
		return
	}
	owned := &broadcastDialog{
		session: dialog, tx: tx, peer: peer, acked: make(chan struct{}, 1), answerDone: make(chan struct{}), ackDone: make(chan struct{}),
	}
	session.dialog = owned
	session.invited <- owned
	s.live.mu.Unlock()
	go func() {
		defer close(owned.ackDone)
		for {
			select {
			case ack, ok := <-tx.Acks():
				if !ok {
					return
				}
				s.handleBroadcastAck(ack, tx)
			case <-tx.Done():
				return
			}
		}
	}()
	defer close(owned.answerDone)
	// handler 必须持有事务直到应答完成，不能返回后再异步 RespondSDP。
	owned.answerErr = dialog.RespondSDP(buildBroadcastAnswer(session.sourceID, session.local, peer.ssrc))
}

func (s *sipServer) handleBroadcastAck(req *sip.Request, tx sip.ServerTransaction) {
	if req == nil || req.Method != sip.ACK || req.CSeq() == nil || req.CSeq().MethodName != sip.ACK {
		return
	}
	id, err := sip.DialogIDFromRequestUAS(req)
	if err != nil {
		return
	}
	s.live.mu.Lock()
	defer s.live.mu.Unlock()
	for _, session := range s.live.broadcasts {
		dialog := session.dialog
		if dialog == nil || dialog.session.ID != id || req.Recipient.User != session.sourceID ||
			dialog.session.LoadState() == sip.DialogStateEnded || !s.devices.registeredSource(session.deviceID, req.Source(), s.now()) {
			continue
		}
		if dialog.session.ReadAck(req, tx) == nil {
			select {
			case dialog.acked <- struct{}{}:
			default:
			}
		}
		return
	}
}

func (s *sipServer) handleBye(req *sip.Request, tx sip.ServerTransaction) {
	id, err := sip.DialogIDFromRequestUAS(req)
	s.live.mu.Lock()
	for _, session := range s.live.broadcasts {
		dialog := session.dialog
		if err != nil || dialog == nil || dialog.session.ID != id {
			continue
		}
		if req.CSeq() == nil || req.CSeq().MethodName != sip.BYE || req.CSeq().SeqNo <= dialog.session.InviteRequest.CSeq().SeqNo ||
			req.Recipient.User != session.sourceID || !s.devices.registeredSource(session.deviceID, req.Source(), s.now()) {
			s.live.mu.Unlock()
			s.respond(tx, sip.NewResponseFromRequest(req, 481, "Call/Transaction Does Not Exist", nil))
			return
		}
		// 先标明远端结束，避免本地清理向设备回送多余 BYE。
		session.cancel(errBroadcastRemoteBye)
		s.live.mu.Unlock()
		if err := dialog.session.ReadBye(req, tx); err != nil {
			s.logger.Warn("broadcast remote BYE failed", "source_id", session.sourceID, "error", err)
		}
		return
	}
	s.live.mu.Unlock()
	s.live.handleRemoteBye(req, tx)
}
