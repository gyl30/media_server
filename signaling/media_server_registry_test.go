package main

import (
	"fmt"
	"sync"
	"testing"
	"time"
)

func TestMediaServerRegistrationAndStableSelection(t *testing.T) {
	registry := newMediaServerRegistry()
	now := time.Date(2026, 8, 29, 12, 0, 0, 0, time.UTC)
	first := testMediaServerRegistration("media-1", "instance-a", "127.0.0.1")
	second := testMediaServerRegistration("media-2", "instance-b", "127.0.0.2")
	if err := registry.register(first, now); err != nil {
		t.Fatalf("register first error = %v", err)
	}
	if err := registry.register(second, now); err != nil {
		t.Fatalf("register second error = %v", err)
	}
	selected, ok := registry.selectOnline()
	if !ok || selected.serverID != first.ServerID || selected.instanceID != first.InstanceID {
		t.Fatalf("selected = %+v, exists = %v", selected, ok)
	}
}

func TestMediaServerRegistrationRejectsDuplicateAndSplitBrain(t *testing.T) {
	registry := newMediaServerRegistry()
	now := time.Now()
	registration := testMediaServerRegistration("media-1", "instance-a", "127.0.0.1")
	if err := registry.register(registration, now); err != nil {
		t.Fatalf("register error = %v", err)
	}
	if err := registry.register(registration, now); err != errMediaServerConflict {
		t.Fatalf("duplicate error = %v", err)
	}
	other := testMediaServerRegistration("media-1", "instance-b", "127.0.0.1")
	if err := registry.register(other, now); err != errMediaServerConflict {
		t.Fatalf("split brain error = %v", err)
	}
}

func TestMediaServerNewInstanceAfterTimeoutKeepsOldFenced(t *testing.T) {
	registry := newMediaServerRegistry()
	now := time.Date(2026, 8, 29, 12, 0, 0, 0, time.UTC)
	old := testMediaServerRegistration("media-1", "instance-a", "127.0.0.1")
	if err := registry.register(old, now); err != nil {
		t.Fatalf("register old error = %v", err)
	}
	offline := registry.expire(now.Add(16*time.Second), 15*time.Second)
	if len(offline) != 1 || offline[0].instanceID != old.InstanceID {
		t.Fatalf("offline = %+v", offline)
	}
	if err := registry.heartbeat(old.ServerID, old.InstanceID, now.Add(17*time.Second)); err != errMediaServerStale {
		t.Fatalf("old heartbeat error = %v", err)
	}
	if err := registry.register(old, now.Add(17*time.Second)); err != errMediaServerConflict {
		t.Fatalf("old registration error = %v", err)
	}
	replacement := testMediaServerRegistration("media-1", "instance-b", "127.0.0.1")
	if err := registry.register(replacement, now.Add(17*time.Second)); err != nil {
		t.Fatalf("replacement error = %v", err)
	}
	selected, ok := registry.selectOnline()
	if !ok || selected.instanceID != replacement.InstanceID {
		t.Fatalf("selected = %+v, exists = %v", selected, ok)
	}
}

func TestMediaServerHeartbeatAndTimeout(t *testing.T) {
	registry := newMediaServerRegistry()
	now := time.Date(2026, 8, 29, 12, 0, 0, 0, time.UTC)
	registration := testMediaServerRegistration("media-1", "instance-a", "127.0.0.1")
	if err := registry.register(registration, now); err != nil {
		t.Fatalf("register error = %v", err)
	}
	if err := registry.heartbeat(registration.ServerID, registration.InstanceID, now.Add(10*time.Second)); err != nil {
		t.Fatalf("heartbeat error = %v", err)
	}
	if offline := registry.expire(now.Add(20*time.Second), 15*time.Second); len(offline) != 0 {
		t.Fatalf("early offline = %+v", offline)
	}
	if offline := registry.expire(now.Add(26*time.Second), 15*time.Second); len(offline) != 1 {
		t.Fatalf("offline = %+v", offline)
	}
	if err := registry.heartbeat("unknown", "unknown", now); err != errMediaServerStale {
		t.Fatalf("unknown heartbeat error = %v", err)
	}
}

func TestMediaServerRegistryConcurrentOperations(t *testing.T) {
	registry := newMediaServerRegistry()
	now := time.Now()
	const count = 64
	var wait sync.WaitGroup
	for index := range count {
		wait.Add(1)
		go func() {
			defer wait.Done()
			id := fmt.Sprintf("media-%d", index)
			registration := testMediaServerRegistration(id, fmt.Sprintf("instance-%d", index), fmt.Sprintf("127.0.1.%d", index+1))
			if err := registry.register(registration, now); err != nil {
				t.Errorf("register %s error = %v", id, err)
				return
			}
			if err := registry.heartbeat(registration.ServerID, registration.InstanceID, now.Add(time.Second)); err != nil {
				t.Errorf("heartbeat %s error = %v", id, err)
			}
		}()
	}
	wait.Wait()
	if registry.onlineCount() != count {
		t.Fatalf("online count = %d", registry.onlineCount())
	}
}

func testMediaServerRegistration(serverID, instanceID, mediaIP string) mediaServerRegistration {
	return mediaServerRegistration{
		ServerID:   serverID,
		InstanceID: instanceID,
		ControlURL: "http://" + mediaIP + ":8080",
		MediaIP:    mediaIP,
	}
}
