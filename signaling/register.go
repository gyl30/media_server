package main

import (
	"context"
	"fmt"
	"net/http"
	"strconv"
	"time"

	"github.com/emiago/sipgo/sip"
)

type registration struct {
	deviceID string
	contact  sip.Uri
	expires  uint32
}

func (s *sipServer) handleRegister(req *sip.Request, tx sip.ServerTransaction) {
	registration, err := parseRegistration(req, s.cfg)
	if err != nil {
		s.respond(tx, sip.NewResponseFromRequest(req, sip.StatusBadRequest, "Bad Request", nil))
		return
	}
	s.live.mu.Lock()
	exists, err := s.deviceStore.exists(context.Background(), registration.deviceID)
	if err != nil {
		s.live.mu.Unlock()
		s.logger.Error("REGISTER allowlist lookup failed", "error", err)
		s.respond(tx, sip.NewResponseFromRequest(req, sip.StatusInternalServerError, "Internal Server Error", nil))
		return
	}
	_, stopping := s.live.stoppingDevices[registration.deviceID]
	if !exists || stopping {
		s.live.mu.Unlock()
		s.respond(tx, sip.NewResponseFromRequest(req, sip.StatusForbidden, "Forbidden", nil))
		return
	}
	authorization := req.GetHeader("Authorization")
	if authorization == nil || !s.auth.verify(registration.deviceID, req.Method.String(), req.Recipient.Addr(), authorization.Value()) {
		s.live.mu.Unlock()
		s.respondUnauthorized(req, tx, registration.deviceID)
		return
	}

	now := s.now()
	shouldQuery := false
	var stopped []*liveSession
	becameOffline := false
	if registration.expires == 0 {
		if s.devices.unregister(registration.deviceID) {
			becameOffline = true
			stopped = s.live.beginDeviceStopLocked(registration.deviceID)
			s.channels.removeDevice(registration.deviceID)
		}
	} else {
		shouldQuery = s.devices.register(registeredDevice{
			id:             registration.deviceID,
			contact:        registration.contact,
			remoteEndpoint: req.Source(),
			expiresAt:      now.Add(time.Duration(registration.expires) * time.Second),
			lastHeartbeat:  now,
			online:         true,
		})
	}
	if shouldQuery {
		device, _ := s.devices.get(registration.deviceID)
		s.enqueueCatalog(device)
	}
	s.live.mu.Unlock()
	if becameOffline {
		s.live.finishDeviceOffline(registration.deviceID, stopped)
	}

	response := sip.NewResponseFromRequest(req, sip.StatusOK, "OK", nil)
	response.AppendHeader(sip.NewHeader("Date", now.UTC().Format(http.TimeFormat)))
	if registration.expires != 0 {
		params := sip.NewParams()
		params.Add("expires", strconv.FormatUint(uint64(registration.expires), 10))
		response.AppendHeader(&sip.ContactHeader{Address: *registration.contact.Clone(), Params: params})
	}
	s.respond(tx, response)
}

func parseRegistration(req *sip.Request, cfg config) (registration, error) {
	from := req.From()
	to := req.To()
	contacts := req.GetHeaders("Contact")
	if from == nil || to == nil || !validDigits(from.Address.User, 20) || to.Address.User != from.Address.User || len(contacts) != 1 {
		return registration{}, fmt.Errorf("invalid REGISTER identity")
	}
	if req.Recipient.User != "" && req.Recipient.User != cfg.sipID {
		return registration{}, fmt.Errorf("invalid REGISTER target")
	}
	contact := req.Contact()
	if contact == nil {
		return registration{}, fmt.Errorf("missing Contact")
	}
	expires, err := registrationExpires(req, contact, cfg.registerExpires)
	if err != nil {
		return registration{}, err
	}
	if contact.Address.Wildcard {
		if expires != 0 {
			return registration{}, fmt.Errorf("wildcard Contact requires zero expiry")
		}
		return registration{deviceID: from.Address.User}, nil
	}
	if contact.Address.Scheme != "sip" || contact.Address.User != from.Address.User || contact.Address.Host == "" {
		return registration{}, fmt.Errorf("invalid Contact")
	}
	return registration{deviceID: from.Address.User, contact: *contact.Address.Clone(), expires: expires}, nil
}

func registrationExpires(req *sip.Request, contact *sip.ContactHeader, defaultExpiry time.Duration) (uint32, error) {
	value, ok := contact.Params.Get("expires")
	if !ok {
		if header := req.GetHeader("Expires"); header != nil {
			value = header.Value()
			ok = true
		}
	}
	if !ok {
		return uint32(defaultExpiry / time.Second), nil
	}
	parsed, err := strconv.ParseUint(value, 10, 32)
	if err != nil {
		return 0, fmt.Errorf("invalid registration expiry")
	}
	return uint32(parsed), nil
}

func (s *sipServer) respondUnauthorized(req *sip.Request, tx sip.ServerTransaction, deviceID string) {
	challenge, err := s.auth.challenge(deviceID)
	if err != nil {
		s.respond(tx, sip.NewResponseFromRequest(req, sip.StatusInternalServerError, "Internal Server Error", nil))
		return
	}
	response := sip.NewResponseFromRequest(req, sip.StatusUnauthorized, "Unauthorized", nil)
	response.AppendHeader(sip.NewHeader("WWW-Authenticate", challenge.String()))
	s.respond(tx, response)
}

func (s *sipServer) respond(tx sip.ServerTransaction, response *sip.Response) {
	if err := tx.Respond(response); err != nil {
		s.logger.Error("SIP response failed", "status", response.StatusCode, "error", err)
	}
}
