package main

import (
	"context"
)

type mediaSender struct {
	engine *mediaEngine
	done   <-chan struct{}
}

func startMediaSender(ctx context.Context, target mediaTarget, source *sharedMediaSource, bindAddress string, loss packetLoss) (*mediaSender, error) {
	engine, err := startMediaEngine(ctx, source, bindAddress, 1, 1, 40, 64, loss)
	if err != nil {
		return nil, err
	}
	if err := engine.add(0, target); err != nil {
		_ = engine.stop()
		return nil, err
	}
	return &mediaSender{engine: engine, done: engine.done}, nil
}

func (s *mediaSender) stop() error {
	return s.engine.stop()
}

func (s *mediaSender) error() error {
	return s.engine.err
}
