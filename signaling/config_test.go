package main

import (
	"testing"
	"time"
)

func TestParseConfig(t *testing.T) {
	t.Run("default", func(t *testing.T) {
		cfg, err := parseConfig(nil)
		if err != nil {
			t.Fatalf("parseConfig() error = %v", err)
		}
		if cfg.sipListen != "0.0.0.0:5060" {
			t.Fatalf("sipListen = %q", cfg.sipListen)
		}
		if cfg.sipID != "34020000002000000001" || cfg.sipDomain != "3402000000" {
			t.Fatalf("SIP identity = %q/%q", cfg.sipID, cfg.sipDomain)
		}
		if cfg.mediaRequestTimeout != 3*time.Second || cfg.inviteTimeout != 10*time.Second || cfg.byeTimeout != 3*time.Second {
			t.Fatalf("operation timeouts = %s/%s/%s", cfg.mediaRequestTimeout, cfg.inviteTimeout, cfg.byeTimeout)
		}
	})

	t.Run("explicit listen address", func(t *testing.T) {
		cfg, err := parseConfig([]string{"-sip-listen", "127.0.0.1:15060"})
		if err != nil {
			t.Fatalf("parseConfig() error = %v", err)
		}
		if cfg.sipListen != "127.0.0.1:15060" {
			t.Fatalf("sipListen = %q", cfg.sipListen)
		}
	})
}

func TestParseConfigRejectsInvalidInput(t *testing.T) {
	for _, args := range [][]string{
		{"-sip-listen", "127.0.0.1"},
		{"-sip-listen", "127.0.0.1:65536"},
		{"-sip-advertise", "0.0.0.0:5060"},
		{"-sip-id", "invalid"},
		{"-sip-domain", "340200"},
		{"-sip-password", ""},
		{"-register-expires", "0s"},
		{"-heartbeat-timeout", "0s"},
		{"-http-listen", "bad"},
		{"-media-server-timeout", "0s"},
		{"-media-request-timeout", "0s"},
		{"-invite-timeout", "0s"},
		{"-bye-timeout", "0s"},
		{"-unknown"},
		{"positional"},
	} {
		if _, err := parseConfig(args); err == nil {
			t.Fatalf("parseConfig(%q) succeeded", args)
		}
	}
}
