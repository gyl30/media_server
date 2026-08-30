package main

import (
	"encoding/binary"
	"net"
	"testing"
	"time"
)

func TestMediaSenderSendsSharedPSOverRTP(t *testing.T) {
	listener, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatalf("ListenUDP() error = %v", err)
	}
	t.Cleanup(func() { _ = listener.Close() })
	source, err := newSharedMediaSource(testAnnexB)
	if err != nil {
		t.Fatalf("newSharedMediaSource() error = %v", err)
	}
	target := mediaTarget{
		address: "127.0.0.1", rtpPort: uint16(listener.LocalAddr().(*net.UDPAddr).Port), payloadType: 96, ssrc: 0x10203040,
	}
	sender, err := startMediaSender(t.Context(), target, source, "127.0.0.1", packetLoss{})
	if err != nil {
		t.Fatalf("startMediaSender() error = %v", err)
	}
	t.Cleanup(func() {
		if err := sender.stop(); err != nil {
			t.Errorf("sender.stop() error = %v", err)
		}
	})
	if err := listener.SetReadDeadline(time.Now().Add(2 * time.Second)); err != nil {
		t.Fatalf("SetReadDeadline() error = %v", err)
	}
	packet := make([]byte, 2048)
	bytesRead, _, err := listener.ReadFromUDP(packet)
	if err != nil {
		t.Fatalf("ReadFromUDP() error = %v", err)
	}
	packet = packet[:bytesRead]
	if len(packet) < 16 || packet[0] != 0x80 || packet[1]&0x7f != 96 {
		t.Fatalf("invalid RTP packet: %x", packet)
	}
	if binary.BigEndian.Uint32(packet[4:8]) != 3600 || binary.BigEndian.Uint32(packet[8:12]) != 0x10203040 {
		t.Fatalf("timestamp=%d ssrc=%x", binary.BigEndian.Uint32(packet[4:8]), binary.BigEndian.Uint32(packet[8:12]))
	}
	if string(packet[12:16]) != "\x00\x00\x01\xba" {
		t.Fatalf("RTP payload is not MPEG-PS: %x", packet[12:])
	}
}
