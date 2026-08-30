package main

import (
	"context"
	"log/slog"
	"net/netip"
	"runtime"
	"time"
)

func runGenerator(ctx context.Context, cfg config, source *sharedMediaSource, logger *slog.Logger) error {
	sink, err := netip.ParseAddrPort(cfg.mediaSink)
	if err != nil {
		return err
	}
	engine, err := startMediaEngine(ctx, source, cfg.mediaBind, cfg.liveCount, cfg.mediaWorkers, cfg.phaseBuckets, cfg.batchSize, packetLoss{percent: cfg.packetLossPercent, seed: cfg.seed})
	if err != nil {
		return err
	}
	for index := range cfg.liveCount {
		if err := engine.add(index, mediaTarget{
			address: sink.Addr().String(), rtpPort: sink.Port(), payloadType: 96, ssrc: uint32(200_000_001 + index),
		}); err != nil {
			_ = engine.stop()
			return err
		}
	}

	started := time.Now()
	reportTicker := time.NewTicker(time.Second)
	defer reportTicker.Stop()
	timer := time.NewTimer(cfg.liveDuration)
	defer timer.Stop()
	for {
		select {
		case <-ctx.Done():
			_ = engine.stop()
			logGeneratorSummary(logger, engine, cfg.liveCount, sink, time.Since(started))
			return ctx.Err()
		case <-reportTicker.C:
			logGeneratorSummary(logger, engine, cfg.liveCount, sink, time.Since(started))
		case <-timer.C:
			err := engine.stop()
			logGeneratorSummary(logger, engine, cfg.liveCount, sink, time.Since(started))
			return err
		}
	}
}

func logGeneratorSummary(logger *slog.Logger, engine *mediaEngine, sessions int, sink netip.AddrPort, elapsed time.Duration) {
	engine.tableMu.RLock()
	active := engine.table.activeCount()
	waiting := engine.table.waitingCount()
	engine.tableMu.RUnlock()
	var memory runtime.MemStats
	runtime.ReadMemStats(&memory)
	packets := engine.counters.packets.Load()
	bytesSent := engine.counters.bytes.Load()
	seconds := elapsed.Seconds()
	logger.Info("simulator summary",
		"mode", "generator",
		"sessions", sessions,
		"active", active,
		"waiting", waiting,
		"sink", sink,
		"elapsed_ms", elapsed.Milliseconds(),
		"rtp_packets", packets,
		"rtp_bytes", bytesSent,
		"rtp_pps", float64(packets)/seconds,
		"rtp_gbps", float64(bytesSent)*8/seconds/1_000_000_000,
		"rtp_dropped", engine.counters.dropped.Load(),
		"send_errors", engine.counters.sendErrors.Load(),
		"phase_drops", engine.counters.phaseDrops.Load(),
		"goroutines", runtime.NumGoroutine(),
		"heap_bytes", memory.HeapAlloc,
		"gc", memory.NumGC,
	)
}
