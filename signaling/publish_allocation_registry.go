package main

import (
	"sync"
	"time"

	"github.com/google/uuid"
)

const publishAllocationTTL = 30 * time.Second

type publishAllocationState string

const (
	publishAllocationPending publishAllocationState = "pending"
	publishAllocationClaimed publishAllocationState = "claimed"
	publishAllocationExpired publishAllocationState = "expired"
)

type publishAllocation struct {
	streamID   string
	direction  string
	protocol   string
	streamName string
	serverID   string
	instanceID string
	createdAt  time.Time
	expiresAt  time.Time
	state      publishAllocationState
}

type publishAllocationRegistry struct {
	mu          sync.RWMutex
	allocations map[string]publishAllocation
}

func newPublishAllocationRegistry() *publishAllocationRegistry {
	return &publishAllocationRegistry{allocations: make(map[string]publishAllocation)}
}

func (r *publishAllocationRegistry) create(protocol, streamName string, server mediaServerInstance, now time.Time) publishAllocation {
	allocation := publishAllocation{
		streamID: uuid.NewString(), direction: "input", protocol: protocol, streamName: streamName,
		serverID: server.serverID, instanceID: server.instanceID, createdAt: now,
		expiresAt: now.Add(publishAllocationTTL), state: publishAllocationPending,
	}
	r.mu.Lock()
	r.allocations[allocation.streamID] = allocation
	r.mu.Unlock()
	return allocation
}

func (r *publishAllocationRegistry) lookup(streamID string) (publishAllocation, bool) {
	r.mu.RLock()
	allocation, ok := r.allocations[streamID]
	r.mu.RUnlock()
	return allocation, ok
}

func (r *publishAllocationRegistry) expire(now time.Time) {
	r.mu.Lock()
	for streamID, allocation := range r.allocations {
		if allocation.state == publishAllocationPending && !now.Before(allocation.expiresAt) {
			allocation.state = publishAllocationExpired
			r.allocations[streamID] = allocation
		}
	}
	r.mu.Unlock()
}
