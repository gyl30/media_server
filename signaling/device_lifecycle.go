package main

import (
	"context"
	"errors"
)

type deviceStopSnapshot struct {
	sessions  []*liveSession
	broadcast *broadcastSession
}

func (s *liveService) beginDeviceStopLocked(deviceID string) deviceStopSnapshot {
	s.stoppingDevices[deviceID] = struct{}{}
	stopped := deviceStopSnapshot{broadcast: s.broadcasts[deviceID]}
	if stopped.broadcast != nil {
		stopped.broadcast.cancel(errDeviceOffline)
	}
	for key, cancel := range s.ptzOwners {
		if key.deviceID == deviceID {
			cancel()
		}
	}
	for _, session := range s.sessions {
		if session.key.deviceID == deviceID {
			s.tokens.revokeStream(session.streamID)
			stopped.sessions = append(stopped.sessions, session)
		}
	}
	return stopped
}

func (s *liveService) stopDeviceSessions(ctx context.Context, stopped deviceStopSnapshot) error {
	var result error
	for _, session := range stopped.sessions {
		s.mu.Lock()
		if err := s.stopSessionLocked(ctx, session); err != nil {
			result = errors.Join(result, err)
		}
	}
	if stopped.broadcast != nil {
		select {
		case <-stopped.broadcast.done:
			result = errors.Join(result, stopped.broadcast.cleanupErr)
		case <-ctx.Done():
			result = errors.Join(result, ctx.Err())
		}
	}
	return result
}

func (s *liveService) finishDeviceOffline(deviceID string, stopped deviceStopSnapshot) {
	if err := s.stopDeviceSessions(context.Background(), stopped); err != nil {
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
	stopped := s.beginDeviceStopLocked(deviceID)
	s.mu.Unlock()
	err := s.stopDeviceSessions(context.WithoutCancel(ctx), stopped)
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
