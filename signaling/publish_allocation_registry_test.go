package main

import (
	"sync"
	"testing"
	"time"

	"github.com/google/uuid"
)

func TestPublishAllocationRegistryExpiresPendingAllocation(t *testing.T) {
	registry := newPublishAllocationRegistry()
	now := time.Date(2026, time.September, 14, 12, 0, 0, 0, time.UTC)
	server := mediaServerInstance{serverID: "media-1", instanceID: "instance-a"}

	allocation := registry.create("rtmp", "live/camera", server, now)
	parsed, err := uuid.Parse(allocation.streamID)
	if err != nil || parsed.Version() != 4 || parsed.String() != allocation.streamID {
		t.Fatalf("stream ID = %q, parse error = %v", allocation.streamID, err)
	}
	if allocation.direction != "input" || allocation.protocol != "rtmp" || allocation.streamName != "live/camera" ||
		allocation.serverID != server.serverID || allocation.instanceID != server.instanceID ||
		allocation.createdAt != now || allocation.expiresAt != now.Add(publishAllocationTTL) || allocation.state != publishAllocationPending {
		t.Fatalf("allocation = %+v", allocation)
	}

	registry.expire(now.Add(publishAllocationTTL - time.Nanosecond))
	allocation, ok := registry.lookup(allocation.streamID)
	if !ok || allocation.state != publishAllocationPending {
		t.Fatalf("allocation before deadline = %+v, ok = %v", allocation, ok)
	}

	registry.expire(now.Add(publishAllocationTTL))
	allocation, ok = registry.lookup(allocation.streamID)
	if !ok || allocation.state != publishAllocationExpired {
		t.Fatalf("allocation at deadline = %+v, ok = %v", allocation, ok)
	}
}

func TestPublishAllocationRegistryGeneratesUniqueRuntimeIDs(t *testing.T) {
	registry := newPublishAllocationRegistry()
	server := mediaServerInstance{serverID: "media-1", instanceID: "instance-a"}
	const count = 64
	ids := make(chan string, count)
	var wait sync.WaitGroup
	for range count {
		wait.Add(1)
		go func() {
			defer wait.Done()
			ids <- registry.create("rtsp", "live/camera", server, time.Now()).streamID
		}()
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
