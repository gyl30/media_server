package main

import (
	"bytes"
	"encoding/binary"
	"net"
	"net/netip"
	"testing"
	"time"
)

func TestUDPBatchSenderSharesPayloadAndKeepsRTPStateIndependent(t *testing.T) {
	receiver, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	defer receiver.Close()
	socket, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	defer socket.Close()

	table := newSessionTable(2, 1, 1)
	destination := receiver.LocalAddr().(*net.UDPAddr).AddrPort()
	for index := range 2 {
		if !table.add(index, destination, 96, uint32(200000001+index)) {
			t.Fatalf("add(%d) failed", index)
		}
	}
	table.slots[1].rtp.timestampOffset = 90_000
	table.activateWaiting()

	payload := bytes.Repeat([]byte{0x5a}, 2*maxRTPPayload+1)
	original := bytes.Clone(payload)
	unit := mediaUnit{timestamp: 3_600, fragments: fragmentPSPayload(payload, maxRTPPayload)}
	sender := newUDPBatchSender(socket, 4, packetLoss{})
	packets, bytesSent, dropped, err := sender.send(table, table.active(0, 0), unit)
	if err != nil {
		t.Fatalf("send() error = %v", err)
	}
	if packets != 6 || bytesSent != 2*(len(payload)+3*12) || dropped != 0 {
		t.Fatalf("send() packets=%d bytes=%d dropped=%d", packets, bytesSent, dropped)
	}
	if !bytes.Equal(payload, original) {
		t.Fatal("shared payload was modified")
	}

	if err := receiver.SetReadDeadline(time.Now().Add(2 * time.Second)); err != nil {
		t.Fatal(err)
	}
	sequences := map[uint32][]uint16{}
	timestamps := map[uint32]uint32{}
	reassembled := map[uint32][]byte{}
	markers := map[uint32][]bool{}
	for range packets {
		packet := make([]byte, 1600)
		length, _, err := receiver.ReadFromUDP(packet)
		if err != nil {
			t.Fatal(err)
		}
		packet = packet[:length]
		ssrc := binary.BigEndian.Uint32(packet[8:12])
		sequences[ssrc] = append(sequences[ssrc], binary.BigEndian.Uint16(packet[2:4]))
		timestamps[ssrc] = binary.BigEndian.Uint32(packet[4:8])
		markers[ssrc] = append(markers[ssrc], packet[1]&0x80 != 0)
		reassembled[ssrc] = append(reassembled[ssrc], packet[12:]...)
	}
	for index := range 2 {
		ssrc := uint32(200000001 + index)
		if !bytes.Equal(reassembled[ssrc], payload) {
			t.Fatalf("SSRC %d payload differs", ssrc)
		}
		if got := sequences[ssrc]; len(got) != 3 || got[0] != 0 || got[1] != 1 || got[2] != 2 {
			t.Fatalf("SSRC %d sequences = %v", ssrc, got)
		}
		wantTimestamp := uint32(3_600 + index*90_000)
		if timestamps[ssrc] != wantTimestamp {
			t.Fatalf("SSRC %d timestamp = %d, want %d", ssrc, timestamps[ssrc], wantTimestamp)
		}
		if got := markers[ssrc]; len(got) != 3 || got[0] || got[1] || got[2] {
			t.Fatalf("SSRC %d markers = %v", ssrc, got)
		}
	}
}

func TestSessionTableStoresCompactAddress(t *testing.T) {
	table := newSessionTable(1, 1, 1)
	destination := netip.MustParseAddrPort("127.0.0.1:40000")
	if !table.add(0, destination, 96, 200000001) || table.slots[0].destination != destination {
		t.Fatal("session destination differs")
	}
}

func TestUDPBatchSenderDropsPacketsDeterministically(t *testing.T) {
	receiver, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	defer receiver.Close()
	socket, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	defer socket.Close()

	table := newSessionTable(1, 1, 1)
	if !table.add(0, receiver.LocalAddr().(*net.UDPAddr).AddrPort(), 96, 200000001) {
		t.Fatal("add(0) failed")
	}
	table.activateWaiting()
	unit := mediaUnit{timestamp: 3_600, fragments: fragmentPSPayload(bytes.Repeat([]byte{0x5a}, 2*maxRTPPayload+1), maxRTPPayload)}
	sender := newUDPBatchSender(socket, 4, packetLoss{percent: 100, seed: 42})
	packets, bytesSent, dropped, err := sender.send(table, table.active(0, 0), unit)
	if err != nil {
		t.Fatalf("send() error = %v", err)
	}
	if packets != 0 || bytesSent != 0 || dropped != 3 || table.slots[0].rtp.sequence != 3 {
		t.Fatalf("send() packets=%d bytes=%d dropped=%d sequence=%d", packets, bytesSent, dropped, table.slots[0].rtp.sequence)
	}
	if err := receiver.SetReadDeadline(time.Now().Add(50 * time.Millisecond)); err != nil {
		t.Fatal(err)
	}
	packet := make([]byte, 1600)
	if _, _, err := receiver.ReadFromUDP(packet); err == nil {
		t.Fatal("receiver got a dropped RTP packet")
	}
}

func BenchmarkUDPBatchSender(b *testing.B) {
	receiver, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		b.Fatal(err)
	}
	b.Cleanup(func() { _ = receiver.Close() })
	socket, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		b.Fatal(err)
	}
	b.Cleanup(func() { _ = socket.Close() })
	go func() {
		packet := make([]byte, 2048)
		for {
			if _, _, err := receiver.ReadFromUDPAddrPort(packet); err != nil {
				return
			}
		}
	}()

	table := newSessionTable(64, 1, 1)
	destination := receiver.LocalAddr().(*net.UDPAddr).AddrPort()
	for index := range 64 {
		if !table.add(index, destination, 96, uint32(200000001+index)) {
			b.Fatalf("add(%d) failed", index)
		}
	}
	table.activateWaiting()
	payload := bytes.Repeat([]byte{0x5a}, 1200)
	unit := mediaUnit{timestamp: 3_600, fragments: fragmentPSPayload(payload, maxRTPPayload)}
	sender := newUDPBatchSender(socket, 64, packetLoss{})
	b.ReportAllocs()
	b.SetBytes(int64(64 * (12 + len(payload))))
	b.ResetTimer()
	for b.Loop() {
		if _, _, _, err := sender.send(table, table.active(0, 0), unit); err != nil {
			b.Fatal(err)
		}
	}
}
