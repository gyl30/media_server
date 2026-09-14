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

	registry.expire(now.Add(2*publishAllocationTTL - time.Nanosecond))
	if _, ok = registry.lookup(allocation.streamID); !ok {
		t.Fatal("expired allocation removed before retention deadline")
	}
	registry.expire(now.Add(2 * publishAllocationTTL))
	if _, ok = registry.lookup(allocation.streamID); ok {
		t.Fatal("expired allocation retained after retention deadline")
	}
}

func TestPublishAllocationRegistryClaimsOnce(t *testing.T) {
	registry := newPublishAllocationRegistry()
	now := time.Date(2026, time.September, 14, 12, 0, 0, 0, time.UTC)
	server := mediaServerInstance{serverID: "media-1", instanceID: "instance-a"}
	allocation := registry.create("rtmp", "live/camera", server, now)
	claim := publishClaim{
		streamID: allocation.streamID, direction: "input", protocol: "rtmp", streamName: "live/camera",
		serverID: "media-1", instanceID: "instance-a",
	}

	if err := registry.claim(claim, now.Add(time.Second)); err != nil {
		t.Fatalf("claim() error = %v", err)
	}
	stored, ok := registry.lookup(allocation.streamID)
	if !ok || stored.state != publishAllocationClaimed {
		t.Fatalf("claimed allocation = %+v, ok = %v", stored, ok)
	}
	if err := registry.claim(claim, now.Add(2*time.Second)); !errors.Is(err, errPublishAllocationConflict) {
		t.Fatalf("duplicate claim error = %v", err)
	}
	registry.expire(now.Add(2 * publishAllocationTTL))
	if _, ok = registry.lookup(allocation.streamID); ok {
		t.Fatal("claimed allocation retained after retention deadline")
	}
}

func TestPublishAllocationRegistryRejectsMismatchedClaims(t *testing.T) {
	now := time.Date(2026, time.September, 14, 12, 0, 0, 0, time.UTC)
	server := mediaServerInstance{serverID: "media-1", instanceID: "instance-a"}

	for name, change := range map[string]func(*publishClaim){
		"direction":   func(claim *publishClaim) { claim.direction = "output" },
		"protocol":    func(claim *publishClaim) { claim.protocol = "rtsp" },
		"stream name": func(claim *publishClaim) { claim.streamName = "live/other" },
		"server":      func(claim *publishClaim) { claim.serverID = "media-2" },
		"instance":    func(claim *publishClaim) { claim.instanceID = "instance-b" },
	} {
		t.Run(name, func(t *testing.T) {
			registry := newPublishAllocationRegistry()
			allocation := registry.create("rtmp", "live/camera", server, now)
			claim := publishClaim{
				streamID: allocation.streamID, direction: "input", protocol: "rtmp", streamName: "live/camera",
				serverID: "media-1", instanceID: "instance-a",
			}
			change(&claim)
			if err := registry.claim(claim, now.Add(time.Second)); !errors.Is(err, errPublishAllocationConflict) {
				t.Fatalf("claim() error = %v", err)
			}
			stored, ok := registry.lookup(allocation.streamID)
			if !ok || stored.state != publishAllocationPending {
				t.Fatalf("allocation after mismatch = %+v, ok = %v", stored, ok)
			}
		})
	}
}

func TestPublishAllocationRegistryRejectsMissingAndExpiredClaims(t *testing.T) {
	registry := newPublishAllocationRegistry()
	now := time.Date(2026, time.September, 14, 12, 0, 0, 0, time.UTC)
	claim := publishClaim{
		streamID: "00000000-0000-4000-8000-000000000001", direction: "input", protocol: "rtmp", streamName: "live/camera",
		serverID: "media-1", instanceID: "instance-a",
	}
	if err := registry.claim(claim, now); !errors.Is(err, errPublishAllocationNotFound) {
		t.Fatalf("missing claim error = %v", err)
	}

	allocation := registry.create("rtmp", "live/camera", mediaServerInstance{serverID: "media-1", instanceID: "instance-a"}, now)
	claim.streamID = allocation.streamID
	if err := registry.claim(claim, now.Add(publishAllocationTTL)); !errors.Is(err, errPublishAllocationExpired) {
		t.Fatalf("expired claim error = %v", err)
	}
	stored, ok := registry.lookup(allocation.streamID)
	if !ok || stored.state != publishAllocationExpired {
		t.Fatalf("expired allocation = %+v, ok = %v", stored, ok)
	}
}

func TestPublishAllocationRegistryClaimIsAtomic(t *testing.T) {
	registry := newPublishAllocationRegistry()
	now := time.Date(2026, time.September, 14, 12, 0, 0, 0, time.UTC)
	allocation := registry.create("rtmp", "live/camera", mediaServerInstance{serverID: "media-1", instanceID: "instance-a"}, now)
	claim := publishClaim{
		streamID: allocation.streamID, direction: "input", protocol: "rtmp", streamName: "live/camera",
		serverID: "media-1", instanceID: "instance-a",
	}
	const count = 32
	results := make(chan error, count)
	var wait sync.WaitGroup
	for range count {
		wait.Add(1)
		go func() {
			defer wait.Done()
			results <- registry.claim(claim, now.Add(time.Second))
		}()
	}
	wait.Wait()
	close(results)

	accepted := 0
	conflicts := 0
	for err := range results {
		switch {
		case err == nil:
			accepted++
		case errors.Is(err, errPublishAllocationConflict):
			conflicts++
		default:
			t.Fatalf("claim() error = %v", err)
		}
	}
	if accepted != 1 || conflicts != count-1 {
		t.Fatalf("accepted = %d conflicts = %d", accepted, conflicts)
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
