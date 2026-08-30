package main

import (
	"context"
	"fmt"
	"log/slog"
	"os"
	"os/signal"
	"syscall"
	"time"
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
	if cfg.devices != 1 || cfg.liveCount != 1 {
		return runFleet(ctx, cfg, source, logger)
	}
	return runSingle(ctx, cfg, source, logger)
}

func runSingle(ctx context.Context, cfg config, source *sharedMediaSource, logger *slog.Logger) error {
	device, err := newSimulatedDevice(cfg, logger)
	if err != nil {
		return err
	}
	if err := device.start(ctx); err != nil {
		device.close()
		return err
	}
	defer device.close()

	requestContext, cancel := context.WithTimeout(ctx, 5*time.Second)
	err = device.register(requestContext)
	cancel()
	if err != nil {
		return err
	}
	select {
	case err := <-device.catalog:
		if err != nil {
			return err
		}
	case <-time.After(5 * time.Second):
		return fmt.Errorf("Catalog query timeout")
	case <-ctx.Done():
		return ctx.Err()
	}
	requestContext, cancel = context.WithTimeout(ctx, 3*time.Second)
	err = device.keepalive(requestContext, 1)
	cancel()
	if err != nil {
		return err
	}
	logger.Info("simulator Keepalive sent", "device_id", cfg.deviceID)

	heartbeatContext, stopHeartbeats := context.WithCancel(ctx)
	defer stopHeartbeats()
	heartbeatErrors := make(chan error, 1)
	go device.runHeartbeats(heartbeatContext, heartbeatErrors)

	control := newControlClient(cfg.controlURL)
	startContext, startCancel := context.WithTimeout(ctx, 15*time.Second)
	started, err := control.startLive(startContext, cfg.deviceID, cfg.channelID)
	startCancel()
	if err != nil {
		return err
	}
	if started.State != "streaming" || started.SSRC == 0 || started.RTPPort == 0 {
		return fmt.Errorf("invalid live start response: %+v", started)
	}
	var target mediaTarget
	select {
	case target = <-device.invite:
	case <-time.After(2 * time.Second):
		return fmt.Errorf("INVITE media target timeout")
	case <-ctx.Done():
		return ctx.Err()
	}
	if target.ssrc != started.SSRC || target.rtpPort != started.RTPPort {
		return fmt.Errorf("INVITE endpoint does not match live response")
	}
	select {
	case <-device.ack:
	case <-time.After(2 * time.Second):
		return fmt.Errorf("ACK timeout")
	case <-ctx.Done():
		return ctx.Err()
	}
	logger.Info("simulator live started", "stream_name", started.StreamName, "rtp", fmt.Sprintf("%s:%d", target.address, target.rtpPort), "ssrc", target.ssrc)

	sender, err := startMediaSender(ctx, target, source, cfg.mediaBind, packetLoss{percent: cfg.packetLossPercent, seed: cfg.seed})
	if err != nil {
		return err
	}
	defer func() { _ = sender.stop() }()
	timer := time.NewTimer(cfg.liveDuration)
	defer timer.Stop()
	stopRequired := true
	select {
	case <-timer.C:
	case <-device.bye:
		stopRequired = false
	case <-sender.done:
		if ctx.Err() != nil {
			return ctx.Err()
		}
		if err := sender.error(); err != nil {
			return fmt.Errorf("RTP sender ended before live duration: %w", err)
		}
		return fmt.Errorf("RTP sender ended before live duration")
	case err := <-heartbeatErrors:
		return fmt.Errorf("Keepalive failed: %w", err)
	case <-ctx.Done():
		return ctx.Err()
	}

	if stopRequired {
		stopContext, stopCancel := context.WithTimeout(context.Background(), 10*time.Second)
		err = control.stopLive(stopContext, cfg.deviceID, cfg.channelID)
		stopCancel()
		if err != nil {
			_ = sender.stop()
			return err
		}
		select {
		case <-device.bye:
		case <-time.After(2 * time.Second):
			_ = sender.stop()
			return fmt.Errorf("BYE timeout")
		}
	}
	if err := sender.stop(); err != nil {
		return err
	}
	logger.Info("simulator live stopped", "stream_name", started.StreamName)
	return nil
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
