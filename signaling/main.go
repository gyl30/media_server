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
	sources, err := openSourceStore(ctx, cfg.database)
	if err != nil {
		return err
	}
	defer sources.close()
	server, err := newSIPServer(cfg, logger)
	if err != nil {
		return err
	}
	defer server.close()
	ssrcs, err := newSSRCAllocator(cfg.sipDomain)
	if err != nil {
		return err
	}
	media := newMediaServerHTTPClient(cfg.mediaServer, cfg.mediaRequestTimeout)
	live := newLiveService(server, media, ssrcs, logger)
	live.inviteTimeout = cfg.inviteTimeout
	live.byeTimeout = cfg.byeTimeout
	infrastructure := newInfrastructureServer(cfg.httpListen, sources, live, media, logger)
	server.onDeviceOffline = func(deviceID string) { live.deviceOffline(context.Background(), deviceID) }
	logger.Info("SIP UDP listening", "address", cfg.sipListen)
	logger.Info("internal HTTP listening", "address", cfg.httpListen)
	runContext, cancel := context.WithCancel(ctx)
	results := make(chan error, 2)
	go func() { results <- server.serve(runContext) }()
	go func() { results <- infrastructure.serve(runContext) }()
	first := <-results
	cancel()
	second := <-results
	infrastructure.shutdownRTSPPulls(context.Background())
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
