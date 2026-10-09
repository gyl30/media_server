package main

import "testing"

func TestSimulatorDoesNotAcceptControllerOptions(t *testing.T) {
	for _, args := range [][]string{
		{"--control-url", "http://127.0.0.1:9090"},
		{"--start-rate", "10"},
		{"--live-count", "1"},
	} {
		if _, err := parseConfig(args); err == nil {
			t.Fatalf("accepted controller option %v", args)
		}
	}
}

func TestSimulatorOptionalPCMAFile(t *testing.T) {
	cfg, err := parseConfig(nil)
	if err != nil {
		t.Fatal(err)
	}
	if cfg.audioFile != "" || cfg.mediaProfile != "normal" {
		t.Fatalf("changed default media: %+v", cfg)
	}
	cfg, err = parseConfig([]string{"--audio-file", "tone.alaw", "--media-profile", "high"})
	if err != nil {
		t.Fatal(err)
	}
	if cfg.audioFile != "tone.alaw" || cfg.mediaProfile != "high" {
		t.Fatalf("unexpected media configuration: %+v", cfg)
	}
}
