package main

import (
	"maps"
	"time"

	"github.com/google/uuid"
)

const playTicketLifetime = 30 * time.Second

type playTicket struct {
	playID    string
	key       liveKey
	liveID    string
	expiresAt time.Time
}

func (s *liveService) newPlayTicket(deviceID, channelID, liveID string) (playTicket, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	key := liveKey{deviceID: deviceID, channelID: channelID}
	if _, stopping := s.stoppingDevices[deviceID]; stopping {
		return playTicket{}, errDeviceStopping
	}
	session := s.sessions[key]
	if session == nil || session.streamID != liveID || session.state != liveStreaming {
		return playTicket{}, errLiveChanged
	}
	ticket := playTicket{playID: uuid.NewString(), key: key, liveID: liveID,
		expiresAt: s.sip.now().Add(playTicketLifetime)}
	s.tickets[ticket.playID] = ticket
	return ticket, nil
}

func (s *liveService) takePlayTicket(playID string) (playTicket, *liveSession, bool) {
	s.mu.Lock()
	defer s.mu.Unlock()
	ticket, ok := s.tickets[playID]
	delete(s.tickets, playID)
	if !ok || !s.sip.now().Before(ticket.expiresAt) {
		return playTicket{}, nil, false
	}
	if _, stopping := s.stoppingDevices[ticket.key.deviceID]; stopping {
		return playTicket{}, nil, false
	}
	session := s.sessions[ticket.key]
	if session == nil || session.streamID != ticket.liveID || session.state != liveStreaming {
		return playTicket{}, nil, false
	}
	session.offers.Add(1)
	return ticket, session, true
}

func (s *liveService) expirePlayTickets(now time.Time) {
	s.mu.Lock()
	defer s.mu.Unlock()
	maps.DeleteFunc(s.tickets, func(_ string, ticket playTicket) bool { return !now.Before(ticket.expiresAt) })
}

func (s *liveService) invalidateLiveTicketsLocked(liveID string) {
	maps.DeleteFunc(s.tickets, func(_ string, ticket playTicket) bool { return ticket.liveID == liveID })
}
