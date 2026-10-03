package main

import (
	"context"
	"log/slog"
	"os"
	"os/signal"
	"syscall"
)

func run(ctx context.Context, args []string, logger *slog.Logger) error {
	cfg, err := parseConfig(args)
	if err != nil {
		return err
	}
	fixture, err := prepareMediaFixture(ctx, cfg)
	if err != nil {
		return err
	}
	defer fixture.cleanup()
	source, err := loadSharedMediaSource(fixture.path)
	if err != nil {
		return err
	}
	if cfg.mediaSink != "" {
		return runGenerator(ctx, cfg, source, logger)
	}
	return runFleet(ctx, cfg, source, logger)
}

func main() {
	logger := slog.New(slog.NewTextHandler(os.Stderr, nil))
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	err := run(ctx, os.Args[1:], logger)
	stop()
	if err != nil {
		logger.Error("simulator stopped", "error", err)
		os.Exit(1)
	}
}
