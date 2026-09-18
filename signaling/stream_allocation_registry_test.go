package main

import (
	"errors"
	"sync"
	"testing"
	"time"

	"github.com/google/uuid"
)

func TestStreamAllocationRegistryExpiresPendingAllocation(t *testing.T) {
	registry := newStreamAllocationRegistry()
	now := time.Date(2026, time.September, 14, 12, 0, 0, 0, time.UTC)
	server := mediaServerInstance{serverID: "media-1", instanceID: "instance-a"}

	streamID := registry.create(streamOperationPublish, "rtmp", "live/camera", server, now)
	parsed, err := uuid.Parse(streamID)
	if err != nil || parsed.Version() != 4 || parsed.String() != streamID {
		t.Fatalf("stream ID = %q, parse error = %v", streamID, err)
	}
	allocation, ok := storedStreamAllocation(registry, streamID)
	if !ok {
		t.Fatal("created allocation is missing")
	}
	if allocation.operation != streamOperationPublish || allocation.protocol != "rtmp" || allocation.streamName != "live/camera" ||
		allocation.serverID != server.serverID || allocation.instanceID != server.instanceID ||
		allocation.expiresAt != now.Add(streamAllocationTTL) {
		t.Fatalf("allocation = %+v", allocation)
	}

	registry.expire(now.Add(streamAllocationTTL - time.Nanosecond))
	allocation, ok = storedStreamAllocation(registry, streamID)
	if !ok {
		t.Fatalf("allocation before deadline = %+v, ok = %v", allocation, ok)
	}

	registry.expire(now.Add(streamAllocationTTL))
	if _, ok = storedStreamAllocation(registry, streamID); ok {
		t.Fatal("expired allocation retained")
	}
}

func TestStreamAllocationRegistryClaimsOnce(t *testing.T) {
	registry := newStreamAllocationRegistry()
	now := time.Date(2026, time.September, 14, 12, 0, 0, 0, time.UTC)
	server := mediaServerInstance{serverID: "media-1", instanceID: "instance-a"}
	streamID := registry.create(streamOperationPublish, "rtmp", "live/camera", server, now)
	if err := registry.claim(streamID, streamOperationPublish, "rtmp", "live/camera", "media-1", "instance-a", now.Add(time.Second)); err != nil {
		t.Fatalf("claim() error = %v", err)
	}
	if _, ok := storedStreamAllocation(registry, streamID); ok {
		t.Fatal("claimed allocation retained")
	}
	if err := registry.claim(streamID, streamOperationPublish, "rtmp", "live/camera", "media-1", "instance-a", now.Add(2*time.Second)); !errors.Is(err, errStreamAllocationNotFound) {
		t.Fatalf("duplicate claim error = %v", err)
	}
}

func TestStreamAllocationRegistryRejectsMismatchedClaims(t *testing.T) {
	now := time.Date(2026, time.September, 14, 12, 0, 0, 0, time.UTC)
	server := mediaServerInstance{serverID: "media-1", instanceID: "instance-a"}

	for name, claim := range map[string]struct {
		operation                                  streamOperation
		protocol, streamName, serverID, instanceID string
	}{
		"operation":   {operation: streamOperationPlay, protocol: "rtmp", streamName: "live/camera", serverID: "media-1", instanceID: "instance-a"},
		"protocol":    {operation: streamOperationPublish, protocol: "rtsp", streamName: "live/camera", serverID: "media-1", instanceID: "instance-a"},
		"stream name": {operation: streamOperationPublish, protocol: "rtmp", streamName: "live/other", serverID: "media-1", instanceID: "instance-a"},
		"server":      {operation: streamOperationPublish, protocol: "rtmp", streamName: "live/camera", serverID: "media-2", instanceID: "instance-a"},
		"instance":    {operation: streamOperationPublish, protocol: "rtmp", streamName: "live/camera", serverID: "media-1", instanceID: "instance-b"},
	} {
		t.Run(name, func(t *testing.T) {
			registry := newStreamAllocationRegistry()
			streamID := registry.create(streamOperationPublish, "rtmp", "live/camera", server, now)
			if err := registry.claim(streamID, claim.operation, claim.protocol, claim.streamName,
				claim.serverID, claim.instanceID, now.Add(time.Second)); !errors.Is(err, errStreamAllocationConflict) {
				t.Fatalf("claim() error = %v", err)
			}
			stored, ok := storedStreamAllocation(registry, streamID)
			if !ok {
				t.Fatalf("allocation after mismatch = %+v, ok = %v", stored, ok)
			}
			if err := registry.claim(streamID, streamOperationPublish, "rtmp", "live/camera", "media-1", "instance-a", now.Add(time.Second)); err != nil {
				t.Fatalf("exact claim after mismatch error = %v", err)
			}
		})
	}
}

func TestStreamAllocationRegistryRejectsMissingAndExpiredClaims(t *testing.T) {
	registry := newStreamAllocationRegistry()
	now := time.Date(2026, time.September, 14, 12, 0, 0, 0, time.UTC)
	missingID := "00000000-0000-4000-8000-000000000001"
	if err := registry.claim(missingID, streamOperationPublish, "rtmp", "live/camera", "media-1", "instance-a", now); !errors.Is(err, errStreamAllocationNotFound) {
		t.Fatalf("missing claim error = %v", err)
	}

	streamID := registry.create(streamOperationPublish, "rtmp", "live/camera", mediaServerInstance{serverID: "media-1", instanceID: "instance-a"}, now)
	if err := registry.claim(streamID, streamOperationPublish, "rtmp", "live/camera", "media-1", "instance-a", now.Add(streamAllocationTTL)); !errors.Is(err, errStreamAllocationNotFound) {
		t.Fatalf("expired claim error = %v", err)
	}
	if _, ok := storedStreamAllocation(registry, streamID); ok {
		t.Fatal("expired allocation retained after claim")
	}
}

func TestStreamAllocationRegistryClaimIsAtomic(t *testing.T) {
	registry := newStreamAllocationRegistry()
	now := time.Date(2026, time.September, 14, 12, 0, 0, 0, time.UTC)
	streamID := registry.create(streamOperationPublish, "rtmp", "live/camera", mediaServerInstance{serverID: "media-1", instanceID: "instance-a"}, now)
	const count = 32
	results := make(chan error, count)
	var wait sync.WaitGroup
	for range count {
		wait.Go(func() {
			results <- registry.claim(streamID, streamOperationPublish, "rtmp", "live/camera", "media-1", "instance-a", now.Add(time.Second))
		})
	}
	wait.Wait()
	close(results)

	accepted := 0
	notFound := 0
	for err := range results {
		switch {
		case err == nil:
			accepted++
		case errors.Is(err, errStreamAllocationNotFound):
			notFound++
		default:
			t.Fatalf("claim() error = %v", err)
		}
	}
	if accepted != 1 || notFound != count-1 {
		t.Fatalf("accepted = %d not found = %d", accepted, notFound)
	}
}

func storedStreamAllocation(registry *streamAllocationRegistry, streamID string) (streamAllocation, bool) {
	registry.mu.Lock()
	defer registry.mu.Unlock()
	allocation, ok := registry.allocations[streamID]
	return allocation, ok
}

func TestStreamAllocationRegistryGeneratesUniqueRuntimeIDs(t *testing.T) {
	registry := newStreamAllocationRegistry()
	server := mediaServerInstance{serverID: "media-1", instanceID: "instance-a"}
	const count = 64
	ids := make(chan string, count)
	var wait sync.WaitGroup
	for range count {
		wait.Go(func() {
			ids <- registry.create(streamOperationPublish, "rtsp", "live/camera", server, time.Now())
		})
	}
	wait.Wait()
	close(ids)

	seen := make(map[string]struct{}, count)
	for streamID := range ids {
		parsed, err := uuid.Parse(streamID)
		if err != nil || parsed.Version() != 4 || parsed.String() != streamID {
			t.Fatalf("stream ID = %q, parse error = %v", streamID, err)
		}
		if _, exists := seen[streamID]; exists {
			t.Fatalf("duplicate stream ID %q", streamID)
		}
		seen[streamID] = struct{}{}
	}
}
