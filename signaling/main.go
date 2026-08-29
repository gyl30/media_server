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
	server, err := newSIPServer(cfg, logger)
	if err != nil {
		return err
	}
	defer server.close()
	registry := newMediaServerRegistry()
	infrastructure := newInfrastructureServer(cfg, registry, logger)
	ssrcs, err := newSSRCAllocator(cfg.sipDomain)
	if err != nil {
		return err
	}
	live := newLiveService(server, registry, newMediaServerHTTPClient(cfg.mediaRequestTimeout), ssrcs, logger)
	live.inviteTimeout = cfg.inviteTimeout
	live.byeTimeout = cfg.byeTimeout
	server.onDeviceOffline = func(deviceID string) { live.deviceOffline(context.Background(), deviceID) }
	infrastructure.onMediaServerOffline = func(instance mediaServerInstance) { live.mediaServerOffline(context.Background(), instance) }
	logger.Info("SIP UDP listening", "address", cfg.sipListen)
	logger.Info("internal HTTP listening", "address", cfg.httpListen)
	runContext, cancel := context.WithCancel(ctx)
	results := make(chan error, 2)
	go func() { results <- server.serve(runContext) }()
	go func() { results <- infrastructure.serve(runContext) }()
	first := <-results
	cancel()
	second := <-results
	live.shutdown(context.Background())
	if first != nil {
		return first
	}
	return second
}

func main() {
	logger := slog.New(slog.NewTextHandler(os.Stderr, nil))
	slog.SetDefault(logger)
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	err := run(ctx, os.Args[1:], logger)
	stop()
	if err != nil {
		logger.Error("signaling stopped", "error", err)
		os.Exit(1)
	}
}
