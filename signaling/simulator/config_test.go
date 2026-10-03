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
