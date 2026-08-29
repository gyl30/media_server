package main

import (
	"fmt"
	"sync"
	"time"
)

type channel struct {
	deviceID string
	id       string
	name     string
	parentID string
	status   string
}

type catalogKey struct {
	deviceID string
	sn       int
}

type pendingCatalog struct {
	expected  int
	channels  map[string]channel
	expiresAt time.Time
}

type channelRegistry struct {
	mu             sync.RWMutex
	channels       map[string]map[string]channel
	pending        map[catalogKey]pendingCatalog
	now            func() time.Time
	pendingTimeout time.Duration
}

func newChannelRegistry() *channelRegistry {
	return &channelRegistry{
		channels:       make(map[string]map[string]channel),
		pending:        make(map[catalogKey]pendingCatalog),
		now:            time.Now,
		pendingTimeout: 10 * time.Second,
	}
}

func (r *channelRegistry) beginQuery(deviceID string, sn int) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.pending[catalogKey{deviceID: deviceID, sn: sn}] = pendingCatalog{
		channels: make(map[string]channel), expiresAt: r.now().Add(r.pendingTimeout),
	}
}

func (r *channelRegistry) cancelQuery(deviceID string, sn int) {
	r.mu.Lock()
	defer r.mu.Unlock()
	delete(r.pending, catalogKey{deviceID: deviceID, sn: sn})
}

func (r *channelRegistry) apply(response catalogResponse) error {
	key := catalogKey{deviceID: response.DeviceID, sn: response.SN}
	r.mu.Lock()
	defer r.mu.Unlock()
	pending, ok := r.pending[key]
	if !ok || response.SumNum < 0 || response.DeviceList.Num != len(response.DeviceList.Items) || response.SumNum < len(response.DeviceList.Items) {
		return fmt.Errorf("invalid Catalog response")
	}
	if !r.now().Before(pending.expiresAt) {
		delete(r.pending, key)
		return fmt.Errorf("expired Catalog response")
	}
	expected := pending.expected
	if expected == 0 {
		expected = response.SumNum
	} else if expected != response.SumNum {
		return fmt.Errorf("inconsistent Catalog SumNum")
	}
	merged := make(map[string]channel, len(pending.channels)+len(response.DeviceList.Items))
	for channelID, existing := range pending.channels {
		merged[channelID] = existing
	}
	for _, item := range response.DeviceList.Items {
		if !validDigits(item.DeviceID, 20) || (item.Status != "ON" && item.Status != "OFF") {
			return fmt.Errorf("invalid Catalog channel")
		}
		merged[item.DeviceID] = channel{
			deviceID: response.DeviceID,
			id:       item.DeviceID,
			name:     item.Name,
			parentID: item.ParentID,
			status:   item.Status,
		}
	}
	if len(merged) > expected {
		return fmt.Errorf("Catalog exceeds SumNum")
	}
	pending.expected = expected
	pending.channels = merged
	if len(merged) == expected {
		r.channels[response.DeviceID] = merged
		delete(r.pending, key)
		return nil
	}
	r.pending[key] = pending
	return nil
}

func (r *channelRegistry) get(deviceID, channelID string) (channel, bool) {
	r.mu.RLock()
	defer r.mu.RUnlock()
	channels := r.channels[deviceID]
	value, ok := channels[channelID]
	return value, ok
}

func (r *channelRegistry) len(deviceID string) int {
	r.mu.RLock()
	defer r.mu.RUnlock()
	return len(r.channels[deviceID])
}

func (r *channelRegistry) removeDevice(deviceID string) {
	r.mu.Lock()
	defer r.mu.Unlock()
	delete(r.channels, deviceID)
	for key := range r.pending {
		if key.deviceID == deviceID {
			delete(r.pending, key)
		}
	}
}

func (r *channelRegistry) expire(now time.Time) {
	r.mu.Lock()
	defer r.mu.Unlock()
	for key, pending := range r.pending {
		if !now.Before(pending.expiresAt) {
			delete(r.pending, key)
		}
	}
}

func (r *channelRegistry) pendingLen() int {
	r.mu.RLock()
	defer r.mu.RUnlock()
	return len(r.pending)
}
