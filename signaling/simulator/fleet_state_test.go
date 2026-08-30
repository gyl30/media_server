package main

import "testing"

func TestIdentitySetGeneratesAndParsesTwentyThousandDevices(t *testing.T) {
	identities, err := newIdentitySet("34020000001320000001", "34020000001310000001", 20_000)
	if err != nil {
		t.Fatalf("newIdentitySet() error = %v", err)
	}
	for _, index := range []int{0, 1, 9_999, 19_999} {
		deviceID := identities.deviceID(index)
		channelID := identities.channelID(index)
		if len(deviceID) != 20 || len(channelID) != 20 {
			t.Fatalf("index %d IDs = %q %q", index, deviceID, channelID)
		}
		if parsed, ok := identities.deviceIndex(deviceID); !ok || parsed != index {
			t.Fatalf("deviceIndex(%q) = %d %t", deviceID, parsed, ok)
		}
		if parsed, ok := identities.channelIndex(channelID); !ok || parsed != index {
			t.Fatalf("channelIndex(%q) = %d %t", channelID, parsed, ok)
		}
	}
	if _, ok := identities.deviceIndex("34020000001329999999"); ok {
		t.Fatal("out-of-range device ID was accepted")
	}
}

func TestScheduleBucketDistributesDevicesWithoutDuplicates(t *testing.T) {
	seen := make([]bool, 103)
	buffer := make([]int, 0, 2)
	for bucket := range 60 {
		indices := scheduleBucket(buffer[:0], len(seen), 60, bucket)
		for _, index := range indices {
			if seen[index] {
				t.Fatalf("device %d scheduled twice", index)
			}
			seen[index] = true
		}
	}
	for index, scheduled := range seen {
		if !scheduled {
			t.Fatalf("device %d was not scheduled", index)
		}
	}
}

func TestWorkerAndPhaseAssignmentIsDeterministic(t *testing.T) {
	for index := range 20_000 {
		worker, phase := assignMedia(index, 8, 40)
		if worker != index%8 || phase != index%40 {
			t.Fatalf("index %d worker=%d phase=%d", index, worker, phase)
		}
	}
}
