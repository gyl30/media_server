package main

import (
	"bytes"
	"encoding/binary"
	"log/slog"
	"net"
	"strings"
	"testing"
	"time"
)

func TestGeneratorSendsIndependentSessionsWithoutSignaling(t *testing.T) {
	listener, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	defer listener.Close()
	source, err := newSharedMediaSource(testAnnexB)
	if err != nil {
		t.Fatal(err)
	}
	var logs bytes.Buffer
	cfg := config{
		liveCount: 4, mediaBind: "127.0.0.1", mediaSink: listener.LocalAddr().String(),
		mediaWorkers: 2, phaseBuckets: 4, batchSize: 4, liveDuration: 100 * time.Millisecond,
	}
	done := make(chan error, 1)
	go func() {
		done <- runGenerator(t.Context(), cfg, source, slog.New(slog.NewTextHandler(&logs, nil)))
	}()

	ssrcs := make(map[uint32]struct{}, cfg.liveCount)
	packet := make([]byte, 1600)
	deadline := time.Now().Add(time.Second)
	for len(ssrcs) != cfg.liveCount {
		if err := listener.SetReadDeadline(deadline); err != nil {
			t.Fatal(err)
		}
		length, _, err := listener.ReadFromUDP(packet)
		if err != nil {
			t.Fatalf("ReadFromUDP() error = %v", err)
		}
		if length < 12 || packet[1]&0x7f != 96 {
			t.Fatalf("invalid RTP packet length=%d header=%x", length, packet[:min(length, 12)])
		}
		ssrcs[binary.BigEndian.Uint32(packet[8:12])] = struct{}{}
	}
	if err := <-done; err != nil {
		t.Fatalf("runGenerator() error = %v", err)
	}
	if output := logs.String(); !strings.Contains(output, "mode=generator") || !strings.Contains(output, "sessions=4") || !strings.Contains(output, "active=4") || !strings.Contains(output, "rtp_pps=") || !strings.Contains(output, "rtp_gbps=") || !strings.Contains(output, "rtp_dropped=0") || !strings.Contains(output, "send_errors=0") {
		t.Fatalf("summary = %s", output)
	}
}
