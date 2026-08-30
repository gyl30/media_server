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

func TestWorkerAndPhaseAssignmentCoversEveryBucket(t *testing.T) {
	for _, dimensions := range []struct {
		workers int
		phases  int
	}{
		{workers: 16, phases: 40},
		{workers: 8, phases: 40},
		{workers: 7, phases: 40},
		{workers: 16, phases: 37},
	} {
		seen := make([]bool, dimensions.workers*dimensions.phases)
		for index := range len(seen) {
			worker, phase := assignMedia(index, dimensions.workers, dimensions.phases)
			if worker < 0 || worker >= dimensions.workers || phase < 0 || phase >= dimensions.phases {
				t.Fatalf("%d workers, %d phases: index %d mapped outside range: worker=%d phase=%d", dimensions.workers, dimensions.phases, index, worker, phase)
			}
			seen[worker*dimensions.phases+phase] = true
		}
		for bucket, assigned := range seen {
			if !assigned {
				t.Fatalf("%d workers, %d phases: bucket %d was not assigned", dimensions.workers, dimensions.phases, bucket)
			}
		}
	}
}

func TestWorkerAndPhaseAssignmentBalancesTwentyThousandSessions(t *testing.T) {
	const workers = 16
	const phases = 40
	counts := make([]int, workers*phases)
	for index := range 20_000 {
		worker, phase := assignMedia(index, workers, phases)
		counts[worker*phases+phase]++
	}
	minimum := counts[0]
	maximum := counts[0]
	for _, count := range counts[1:] {
		minimum = min(minimum, count)
		maximum = max(maximum, count)
	}
	if maximum-minimum > 1 {
		t.Fatalf("worker/phase occupancy range = %d..%d", minimum, maximum)
	}
}

func TestWorkerAndPhaseAssignmentIsDeterministicAndInRange(t *testing.T) {
	for _, dimensions := range []struct {
		workers  int
		phases   int
		sessions int
	}{
		{workers: 16, phases: 40, sessions: 8},
		{workers: 16, phases: 40, sessions: 40},
		{workers: 7, phases: 40, sessions: 1_000},
		{workers: 16, phases: 37, sessions: 1_000},
	} {
		for index := range dimensions.sessions {
			worker, phase := assignMedia(index, dimensions.workers, dimensions.phases)
			repeatedWorker, repeatedPhase := assignMedia(index, dimensions.workers, dimensions.phases)
			if worker != repeatedWorker || phase != repeatedPhase {
				t.Fatalf("index %d mapping changed from %d/%d to %d/%d", index, worker, phase, repeatedWorker, repeatedPhase)
			}
			if worker < 0 || worker >= dimensions.workers || phase < 0 || phase >= dimensions.phases {
				t.Fatalf("index %d mapped outside range: worker=%d phase=%d", index, worker, phase)
			}
		}
	}
}
