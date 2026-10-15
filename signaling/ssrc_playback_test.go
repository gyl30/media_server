package main

import (
	"sync"
	"testing"
)

func TestSSRCMixedPoolExhaustionAndRelease(t *testing.T) {
	allocator, err := newSSRCAllocator("3402000000")
	if err != nil {
		t.Fatal(err)
	}
	allocated := make([]uint32, 9999)
	seen := make(map[uint32]bool)
	for i := range allocated {
		kind := liveReceiver
		if i%2 != 0 {
			kind = playbackReceiver
		}
		ssrc, err := allocator.acquire(kind)
		if err != nil {
			t.Fatal(err)
		}
		if ssrc/1000000000 != uint32(i%2) || (ssrc%1000000000)/10000 != 20000 || seen[ssrc%10000] || ssrc%10000 == 0 {
			t.Fatalf("invalid or overlapping SSRC %010d", ssrc)
		}
		allocated[i], seen[ssrc%10000] = ssrc, true
	}
	for _, kind := range []receiverKind{liveReceiver, playbackReceiver} {
		if _, err := allocator.acquire(kind); err == nil {
			t.Fatal("real-time and playback must share the exhausted pool")
		}
	}
	allocator.release(allocated[1])
	replacement, err := allocator.acquire(liveReceiver)
	if err != nil || replacement%10000 != allocated[1]%10000 || replacement/1000000000 != 0 {
		t.Fatalf("released historical suffix not reused by live: %010d %v", replacement, err)
	}
	allocator.release(replacement)
	for _, ssrc := range allocated {
		allocator.release(ssrc)
	}
	if len(allocator.active) != 0 {
		t.Fatal("SSRCs remain allocated")
	}
}

func TestSSRCMixedConcurrentAllocation(t *testing.T) {
	allocator, err := newSSRCAllocator("3402000000")
	if err != nil {
		t.Fatal(err)
	}
	values := make(chan uint32, 1600)
	errors := make(chan error, 16)
	var workers sync.WaitGroup
	for worker := range 16 {
		workers.Go(func() {
			kind := liveReceiver
			if worker%2 != 0 {
				kind = playbackReceiver
			}
			for range 100 {
				ssrc, err := allocator.acquire(kind)
				if err != nil {
					errors <- err
					return
				}
				values <- ssrc
			}
		})
	}
	workers.Wait()
	close(values)
	close(errors)
	for err := range errors {
		t.Error(err)
	}
	seen := make(map[uint32]bool)
	for ssrc := range values {
		if seen[ssrc%10000] {
			t.Errorf("overlapping suffix: %010d", ssrc)
		}
		seen[ssrc%10000] = true
		allocator.release(ssrc)
	}
	if len(seen) != 1600 || len(allocator.active) != 0 {
		t.Fatalf("allocated=%d retained=%d", len(seen), len(allocator.active))
	}
}
