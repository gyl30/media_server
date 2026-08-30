package main

import (
	"context"
	"sync"
	"time"
)

func runIndices(ctx context.Context, total, workers, rate int, operation func(int) error) error {
	runContext, cancel := context.WithCancelCause(ctx)
	defer cancel(nil)
	jobs := make(chan int)
	var workerGroup sync.WaitGroup
	for range min(total, workers) {
		workerGroup.Add(1)
		go func() {
			defer workerGroup.Done()
			for {
				select {
				case <-runContext.Done():
					return
				case index, ok := <-jobs:
					if !ok {
						return
					}
					if err := operation(index); err != nil {
						cancel(err)
						return
					}
				}
			}
		}()
	}

	var ticker *time.Ticker
	if rate > 0 {
		ticker = time.NewTicker(max(time.Second/time.Duration(rate), time.Nanosecond))
		defer ticker.Stop()
	}
sendLoop:
	for index := range total {
		if ticker != nil && index != 0 {
			select {
			case <-runContext.Done():
				break sendLoop
			case <-ticker.C:
			}
		}
		select {
		case <-runContext.Done():
			break sendLoop
		case jobs <- index:
		}
	}
	close(jobs)
	workerGroup.Wait()
	return context.Cause(runContext)
}
