package main

import (
	"errors"
	"sync"
	"time"

	"github.com/google/uuid"
)

const publishAllocationTTL = 30 * time.Second

var (
	errPublishAllocationNotFound = errors.New("publish allocation not found")
	errPublishAllocationExpired  = errors.New("publish allocation expired")
	errPublishAllocationConflict = errors.New("publish allocation conflict")
)

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

type publishClaim struct {
	streamID   string
	direction  string
	protocol   string
	streamName string
	serverID   string
	instanceID string
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

func (r *publishAllocationRegistry) claim(claim publishClaim, now time.Time) error {
	r.mu.Lock()
	defer r.mu.Unlock()
	allocation, ok := r.allocations[claim.streamID]
	if !ok {
		return errPublishAllocationNotFound
	}
	if allocation.state == publishAllocationExpired {
		return errPublishAllocationExpired
	}
	if allocation.state != publishAllocationPending {
		return errPublishAllocationConflict
	}
	if !now.Before(allocation.expiresAt) {
		allocation.state = publishAllocationExpired
		r.allocations[claim.streamID] = allocation
		return errPublishAllocationExpired
	}
	if allocation.direction != claim.direction || allocation.protocol != claim.protocol || allocation.streamName != claim.streamName ||
		allocation.serverID != claim.serverID || allocation.instanceID != claim.instanceID {
		return errPublishAllocationConflict
	}
	allocation.state = publishAllocationClaimed
	r.allocations[claim.streamID] = allocation
	return nil
}

func (r *publishAllocationRegistry) expire(now time.Time) {
	r.mu.Lock()
	for streamID, allocation := range r.allocations {
		if allocation.state == publishAllocationPending && !now.Before(allocation.expiresAt) {
			allocation.state = publishAllocationExpired
			r.allocations[streamID] = allocation
		}
		if allocation.state != publishAllocationPending && !now.Before(allocation.expiresAt.Add(publishAllocationTTL)) {
			delete(r.allocations, streamID)
		}
	}
	r.mu.Unlock()
}
