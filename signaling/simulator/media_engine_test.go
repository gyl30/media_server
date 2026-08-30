package main

import (
	"encoding/binary"
	"net"
	"testing"
	"time"
)

func TestMediaEnginePhaseCountDoesNotChangeFrameRate(t *testing.T) {
	source, err := newSharedMediaSource(testAnnexB)
	if err != nil {
		t.Fatal(err)
	}
	engine, err := startMediaEngine(t.Context(), source, "127.0.0.1", 1, 1, 4, 4, packetLoss{})
	if err != nil {
		t.Fatal(err)
	}
	time.Sleep(125 * time.Millisecond)
	if err := engine.stop(); err != nil {
		t.Fatal(err)
	}
	if source.frames < 3 || source.frames > 4 {
		t.Fatalf("source frames in 125ms = %d", source.frames)
	}
}

func TestMediaEngineFansOutWithFixedWorkers(t *testing.T) {
	source, err := newSharedMediaSource(testAnnexB)
	if err != nil {
		t.Fatal(err)
	}
	engine, err := startMediaEngine(t.Context(), source, "127.0.0.2", 4, 2, 4, 4, packetLoss{})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		if err := engine.stop(); err != nil {
			t.Errorf("engine.stop() error = %v", err)
		}
	})

	listeners := make([]*net.UDPConn, 4)
	for index := range listeners {
		listener, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
		if err != nil {
			t.Fatal(err)
		}
		listeners[index] = listener
		t.Cleanup(func() { _ = listener.Close() })
		target := mediaTarget{
			address: "127.0.0.1", rtpPort: uint16(listener.LocalAddr().(*net.UDPAddr).Port), payloadType: 96, ssrc: uint32(200000001 + index),
		}
		if err := engine.add(index, target); err != nil {
			t.Fatalf("add(%d) error = %v", index, err)
		}
	}
	if len(engine.workers) != 2 {
		t.Fatalf("workers = %d", len(engine.workers))
	}
	for index := range engine.workers {
		if address := engine.workers[index].socket.LocalAddr().(*net.UDPAddr); !address.IP.Equal(net.IPv4(127, 0, 0, 2)) {
			t.Fatalf("worker %d bind address = %s", index, address)
		}
		if capacity := cap(engine.workers[index].tasks); capacity != 2 {
			t.Fatalf("worker %d task queue capacity = %d", index, capacity)
		}
	}

	for index, listener := range listeners {
		if err := listener.SetReadDeadline(time.Now().Add(time.Second)); err != nil {
			t.Fatal(err)
		}
		packet := make([]byte, 1600)
		length, _, err := listener.ReadFromUDP(packet)
		if err != nil {
			t.Fatalf("device %d ReadFromUDP() error = %v", index, err)
		}
		packet = packet[:length]
		if got := binary.BigEndian.Uint32(packet[8:12]); got != uint32(200000001+index) {
			t.Fatalf("device %d SSRC = %d", index, got)
		}
		if !engine.remove(index) || engine.remove(index) {
			t.Fatalf("device %d remove result is wrong", index)
		}
	}
}
