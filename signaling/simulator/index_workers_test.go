package main

import (
	"errors"
	"sync"
	"testing"
)

func TestRunIndicesUsesFixedWorkersAndVisitsEveryIndex(t *testing.T) {
	const total = 20
	const workers = 4
	started := make(chan struct{}, total)
	release := make(chan struct{})
	seen := make([]bool, total)
	var mutex sync.Mutex
	done := make(chan error, 1)
	go func() {
		done <- runIndices(t.Context(), total, workers, 0, func(index int) error {
			started <- struct{}{}
			<-release
			mutex.Lock()
			seen[index] = true
			mutex.Unlock()
			return nil
		})
	}()
	for range workers {
		<-started
	}
	select {
	case <-started:
		t.Fatal("more than the fixed worker count ran concurrently")
	default:
	}
	close(release)
	if err := <-done; err != nil {
		t.Fatalf("runIndices() error = %v", err)
	}
	for index, visited := range seen {
		if !visited {
			t.Fatalf("index %d was not visited", index)
		}
	}
}

func TestRunIndicesReturnsOperationError(t *testing.T) {
	want := errors.New("operation failed")
	err := runIndices(t.Context(), 20, 4, 0, func(index int) error {
		if index == 3 {
			return want
		}
		return nil
	})
	if !errors.Is(err, want) {
		t.Fatalf("runIndices() error = %v", err)
	}
}
