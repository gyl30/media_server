package main

func (s *liveService) len() int {
	s.mu.Lock()
	defer s.mu.Unlock()
	return len(s.sessions)
}

func (a *ssrcAllocator) activeCount() int {
	a.mu.Lock()
	defer a.mu.Unlock()
	return len(a.active)
}

func (r *channelRegistry) len(deviceID string) int {
	r.mu.RLock()
	defer r.mu.RUnlock()
	return len(r.channels[deviceID])
}

func (r *channelRegistry) pendingLen() int {
	r.mu.RLock()
	defer r.mu.RUnlock()
	return len(r.pending)
}

func (r *deviceRegistry) len() int {
	r.mu.RLock()
	defer r.mu.RUnlock()
	return len(r.devices)
}
