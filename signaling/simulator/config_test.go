package main

import "testing"

func TestParseConfig(t *testing.T) {
	cfg, err := parseConfig([]string{
		"--listen", "127.0.0.1:0",
	})
	if err != nil {
		t.Fatalf("parseConfig() error = %v", err)
	}
	if cfg.deviceID != "34020000001320000001" || cfg.channelID != "34020000001320000002" || cfg.mediaBind != "127.0.0.1" || cfg.mediaProfile != "normal" {
		t.Fatalf("config = %+v", cfg)
	}
	if cfg.devices != 1 || cfg.liveCount != 1 || cfg.sipEndpoints != 1 || cfg.mediaWorkers != 16 || cfg.phaseBuckets != 40 || cfg.batchSize != 64 || cfg.registerRate != 200 {
		t.Fatalf("scale config = %+v", cfg)
	}
}

func TestParseScaleConfig(t *testing.T) {
	cfg, err := parseConfig([]string{
		"--listen", "127.0.0.1:0",
		"--devices", "20000",
		"--live-count", "9000",
		"--sip-endpoints", "8",
		"--control-workers", "32",
		"--media-workers", "16",
		"--media-bind", "127.0.0.2",
		"--media-sink", "127.0.0.1:40000",
		"--media-profile", "high",
		"--phase-buckets", "40",
		"--batch-size", "128",
		"--register-rate", "500",
		"--start-rate", "500",
		"--packet-loss-percent", "5",
		"--seed", "42",
	})
	if err != nil {
		t.Fatalf("parseConfig() error = %v", err)
	}
	if cfg.devices != 20_000 || cfg.liveCount != 9_000 || cfg.sipEndpoints != 8 || cfg.controlWorkers != 32 || cfg.mediaWorkers != 16 || cfg.mediaBind != "127.0.0.2" || cfg.mediaSink != "127.0.0.1:40000" || cfg.mediaProfile != "high" || cfg.batchSize != 128 || cfg.registerRate != 500 || cfg.startRate != 500 || cfg.packetLossPercent != 5 || cfg.seed != 42 {
		t.Fatalf("config = %+v", cfg)
	}
}

func TestParseConfigRejectsInvalidIdentity(t *testing.T) {
	for name, args := range map[string][]string{
		"bad device":           {"--device-id", "bad"},
		"unspecified":          {"--listen", "0.0.0.0:5062"},
		"hostname listen":      {"--listen", "localhost:5062"},
		"too many devices":     {"--devices", "20001"},
		"too many lives":       {"--devices", "10", "--live-count", "11"},
		"too many endpoints":   {"--devices", "4", "--sip-endpoints", "8"},
		"bad batch":            {"--batch-size", "0"},
		"too many phases":      {"--phase-buckets", "41"},
		"bad media bind":       {"--media-bind", "not-an-ip"},
		"unspecified media":    {"--media-bind", "0.0.0.0"},
		"bad media sink":       {"--media-sink", "localhost:40000"},
		"bad media profile":    {"--media-profile", "maximum"},
		"zero generator":       {"--live-count", "0", "--media-sink", "127.0.0.1:40000"},
		"bad register rate":    {"--register-rate", "-1"},
		"bad packet loss":      {"--packet-loss-percent", "101"},
		"fractional heartbeat": {"--heartbeat", "1500ms"},
	} {
		t.Run(name, func(t *testing.T) {
			if _, err := parseConfig(args); err == nil {
				t.Fatal("parseConfig() succeeded")
			}
		})
	}
}
