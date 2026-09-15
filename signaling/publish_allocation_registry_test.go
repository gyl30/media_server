package main

import (
	"errors"
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
	if allocation.protocol != "rtmp" || allocation.streamName != "live/camera" ||
		allocation.serverID != server.serverID || allocation.instanceID != server.instanceID ||
		allocation.expiresAt != now.Add(publishAllocationTTL) {
		t.Fatalf("allocation = %+v", allocation)
	}

	registry.expire(now.Add(publishAllocationTTL - time.Nanosecond))
	allocation, ok := storedPublishAllocation(registry, allocation.streamID)
	if !ok {
		t.Fatalf("allocation before deadline = %+v, ok = %v", allocation, ok)
	}

	registry.expire(now.Add(publishAllocationTTL))
	if _, ok = storedPublishAllocation(registry, allocation.streamID); ok {
		t.Fatal("expired allocation retained")
	}
}

func TestPublishAllocationRegistryClaimsOnce(t *testing.T) {
	registry := newPublishAllocationRegistry()
	now := time.Date(2026, time.September, 14, 12, 0, 0, 0, time.UTC)
	server := mediaServerInstance{serverID: "media-1", instanceID: "instance-a"}
	allocation := registry.create("rtmp", "live/camera", server, now)
	if err := registry.claim(allocation.streamID, "rtmp", "live/camera", "media-1", "instance-a", now.Add(time.Second)); err != nil {
		t.Fatalf("claim() error = %v", err)
	}
	if _, ok := storedPublishAllocation(registry, allocation.streamID); ok {
		t.Fatal("claimed allocation retained")
	}
	if err := registry.claim(allocation.streamID, "rtmp", "live/camera", "media-1", "instance-a", now.Add(2*time.Second)); !errors.Is(err, errPublishAllocationNotFound) {
		t.Fatalf("duplicate claim error = %v", err)
	}
}

func TestPublishAllocationRegistryRejectsMismatchedClaims(t *testing.T) {
	now := time.Date(2026, time.September, 14, 12, 0, 0, 0, time.UTC)
	server := mediaServerInstance{serverID: "media-1", instanceID: "instance-a"}

	for name, claim := range map[string]struct {
		protocol, streamName, serverID, instanceID string
	}{
		"protocol":    {protocol: "rtsp", streamName: "live/camera", serverID: "media-1", instanceID: "instance-a"},
		"stream name": {protocol: "rtmp", streamName: "live/other", serverID: "media-1", instanceID: "instance-a"},
		"server":      {protocol: "rtmp", streamName: "live/camera", serverID: "media-2", instanceID: "instance-a"},
		"instance":    {protocol: "rtmp", streamName: "live/camera", serverID: "media-1", instanceID: "instance-b"},
	} {
		t.Run(name, func(t *testing.T) {
			registry := newPublishAllocationRegistry()
			allocation := registry.create("rtmp", "live/camera", server, now)
			if err := registry.claim(allocation.streamID, claim.protocol, claim.streamName,
				claim.serverID, claim.instanceID, now.Add(time.Second)); !errors.Is(err, errPublishAllocationConflict) {
				t.Fatalf("claim() error = %v", err)
			}
			stored, ok := storedPublishAllocation(registry, allocation.streamID)
			if !ok {
				t.Fatalf("allocation after mismatch = %+v, ok = %v", stored, ok)
			}
			if err := registry.claim(allocation.streamID, "rtmp", "live/camera", "media-1", "instance-a", now.Add(time.Second)); err != nil {
				t.Fatalf("exact claim after mismatch error = %v", err)
			}
		})
	}
}

func TestPublishAllocationRegistryRejectsMissingAndExpiredClaims(t *testing.T) {
	registry := newPublishAllocationRegistry()
	now := time.Date(2026, time.September, 14, 12, 0, 0, 0, time.UTC)
	missingID := "00000000-0000-4000-8000-000000000001"
	if err := registry.claim(missingID, "rtmp", "live/camera", "media-1", "instance-a", now); !errors.Is(err, errPublishAllocationNotFound) {
		t.Fatalf("missing claim error = %v", err)
	}

	allocation := registry.create("rtmp", "live/camera", mediaServerInstance{serverID: "media-1", instanceID: "instance-a"}, now)
	if err := registry.claim(allocation.streamID, "rtmp", "live/camera", "media-1", "instance-a", now.Add(publishAllocationTTL)); !errors.Is(err, errPublishAllocationNotFound) {
		t.Fatalf("expired claim error = %v", err)
	}
	if _, ok := storedPublishAllocation(registry, allocation.streamID); ok {
		t.Fatal("expired allocation retained after claim")
	}
}

func TestPublishAllocationRegistryClaimIsAtomic(t *testing.T) {
	registry := newPublishAllocationRegistry()
	now := time.Date(2026, time.September, 14, 12, 0, 0, 0, time.UTC)
	allocation := registry.create("rtmp", "live/camera", mediaServerInstance{serverID: "media-1", instanceID: "instance-a"}, now)
	const count = 32
	results := make(chan error, count)
	var wait sync.WaitGroup
	for range count {
		wait.Add(1)
		go func() {
			defer wait.Done()
			results <- registry.claim(allocation.streamID, "rtmp", "live/camera", "media-1", "instance-a", now.Add(time.Second))
		}()
	}
	wait.Wait()
	close(results)

	accepted := 0
	notFound := 0
	for err := range results {
		switch {
		case err == nil:
			accepted++
		case errors.Is(err, errPublishAllocationNotFound):
			notFound++
		default:
			t.Fatalf("claim() error = %v", err)
		}
	}
	if accepted != 1 || notFound != count-1 {
		t.Fatalf("accepted = %d not found = %d", accepted, notFound)
	}
}

func storedPublishAllocation(registry *publishAllocationRegistry, streamID string) (publishAllocation, bool) {
	registry.mu.Lock()
	defer registry.mu.Unlock()
	allocation, ok := registry.allocations[streamID]
	return allocation, ok
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
