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
	errPublishAllocationConflict = errors.New("publish allocation conflict")
)

type publishAllocation struct {
	streamID   string
	protocol   string
	streamName string
	serverID   string
	instanceID string
	expiresAt  time.Time
}

type publishAllocationRegistry struct {
	mu          sync.Mutex
	allocations map[string]publishAllocation
}

func newPublishAllocationRegistry() *publishAllocationRegistry {
	return &publishAllocationRegistry{allocations: make(map[string]publishAllocation)}
}

func (r *publishAllocationRegistry) create(protocol, streamName string, server mediaServerInstance, now time.Time) publishAllocation {
	allocation := publishAllocation{
		streamID: uuid.NewString(), protocol: protocol, streamName: streamName,
		serverID: server.serverID, instanceID: server.instanceID, expiresAt: now.Add(publishAllocationTTL),
	}
	r.mu.Lock()
	r.allocations[allocation.streamID] = allocation
	r.mu.Unlock()
	return allocation
}

func (r *publishAllocationRegistry) claim(streamID, protocol, streamName, serverID, instanceID string, now time.Time) error {
	r.mu.Lock()
	defer r.mu.Unlock()
	allocation, ok := r.allocations[streamID]
	if !ok {
		return errPublishAllocationNotFound
	}
	if !now.Before(allocation.expiresAt) {
		delete(r.allocations, streamID)
		return errPublishAllocationNotFound
	}
	if allocation.protocol != protocol || allocation.streamName != streamName ||
		allocation.serverID != serverID || allocation.instanceID != instanceID {
		return errPublishAllocationConflict
	}
	delete(r.allocations, streamID)
	return nil
}

func (r *publishAllocationRegistry) expire(now time.Time) {
	r.mu.Lock()
	for streamID, allocation := range r.allocations {
		if !now.Before(allocation.expiresAt) {
			delete(r.allocations, streamID)
		}
	}
	r.mu.Unlock()
}
