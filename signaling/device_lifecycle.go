package main

import (
	"context"
	"errors"
)

// Caller holds mu: the fence and session snapshot must precede new play or REGISTER.
func (s *liveService) beginDeviceStopLocked(deviceID string) []*liveSession {
	s.stoppingDevices[deviceID] = struct{}{}
	var sessions []*liveSession
	for _, session := range s.sessions {
		if session.key.deviceID == deviceID {
			s.tokens.revokeStream(session.streamID)
			sessions = append(sessions, session)
		}
	}
	return sessions
}

func (s *liveService) stopDeviceSessions(ctx context.Context, sessions []*liveSession) error {
	var result error
	for _, session := range sessions {
		s.mu.Lock()
		if err := s.stopSessionLocked(ctx, session); err != nil {
			result = errors.Join(result, err)
		}
	}
	return result
}

func (s *liveService) finishDeviceOffline(deviceID string, sessions []*liveSession) {
	if err := s.stopDeviceSessions(context.Background(), sessions); err != nil {
		s.logger.Warn("offline device cleanup failed", "device_id", deviceID, "error", err)
	}
	s.mu.Lock()
	delete(s.stoppingDevices, deviceID)
	s.mu.Unlock()
}

func (s *liveService) deleteDevice(ctx context.Context, deviceID string) error {
	s.mu.Lock()
	if _, err := s.sip.deviceStore.get(ctx, deviceID); err != nil {
		s.mu.Unlock()
		return err
	}
	if _, stopping := s.stoppingDevices[deviceID]; stopping {
		s.mu.Unlock()
		return errDeviceStopping
	}
	sessions := s.beginDeviceStopLocked(deviceID)
	s.mu.Unlock()
	err := s.stopDeviceSessions(ctx, sessions)
	s.mu.Lock()
	defer s.mu.Unlock()
	defer delete(s.stoppingDevices, deviceID)
	if err != nil {
		return err
	}
	if err := s.sip.deviceStore.delete(ctx, deviceID); err != nil {
		return err
	}
	s.sip.devices.unregister(deviceID)
	s.sip.channels.removeDevice(deviceID)
	return nil
}
