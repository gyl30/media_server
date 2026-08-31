package main

import "testing"

func TestParseConfigRejectsDynamicSIPListenPort(t *testing.T) {
	if _, err := parseConfig([]string{"-sip-listen", "127.0.0.1:0"}); err == nil {
		t.Fatal("expected dynamic SIP listen port to be rejected")
	}
}
