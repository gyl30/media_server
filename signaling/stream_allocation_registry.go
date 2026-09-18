package main

import (
	"errors"
	"sync"
	"time"

	"github.com/google/uuid"
)

const streamAllocationTTL = 30 * time.Second

var (
	errStreamAllocationNotFound = errors.New("stream allocation not found")
	errStreamAllocationConflict = errors.New("stream allocation conflict")
)

type streamOperation string

const (
	streamOperationPublish streamOperation = "publish"
	streamOperationPlay    streamOperation = "play"
)

type streamAllocation struct {
	operation  streamOperation
	protocol   string
	streamName string
	serverID   string
	instanceID string
	expiresAt  time.Time
}

type streamAllocationRegistry struct {
	mu          sync.Mutex
	allocations map[string]streamAllocation
}

func newStreamAllocationRegistry() *streamAllocationRegistry {
	return &streamAllocationRegistry{allocations: make(map[string]streamAllocation)}
}

func (r *streamAllocationRegistry) create(operation streamOperation, protocol, streamName string, server mediaServerInstance, now time.Time) string {
	streamID := uuid.NewString()
	allocation := streamAllocation{
		operation: operation,
		protocol:  protocol, streamName: streamName,
		serverID: server.serverID, instanceID: server.instanceID, expiresAt: now.Add(streamAllocationTTL),
	}
	r.mu.Lock()
	r.allocations[streamID] = allocation
	r.mu.Unlock()
	return streamID
}

func (r *streamAllocationRegistry) claim(
	streamID string, operation streamOperation, protocol, streamName, serverID, instanceID string, now time.Time,
) error {
	r.mu.Lock()
	defer r.mu.Unlock()
	allocation, ok := r.allocations[streamID]
	if !ok {
		return errStreamAllocationNotFound
	}
	if !now.Before(allocation.expiresAt) {
		delete(r.allocations, streamID)
		return errStreamAllocationNotFound
	}
	if allocation.operation != operation || allocation.protocol != protocol || allocation.streamName != streamName ||
		allocation.serverID != serverID || allocation.instanceID != instanceID {
		return errStreamAllocationConflict
	}
	delete(r.allocations, streamID)
	return nil
}

func (r *streamAllocationRegistry) expire(now time.Time) {
	r.mu.Lock()
	for streamID, allocation := range r.allocations {
		if !now.Before(allocation.expiresAt) {
			delete(r.allocations, streamID)
		}
	}
	r.mu.Unlock()
}
