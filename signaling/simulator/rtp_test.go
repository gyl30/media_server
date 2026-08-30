package main

import (
	"bytes"
	"encoding/binary"
	"testing"
)

func TestFragmentPSPayloadSharesBytesWithoutFalseDiscontinuity(t *testing.T) {
	payload := make([]byte, 2801)
	for index := range payload {
		payload[index] = byte(index)
	}
	fragments := fragmentPSPayload(payload, 1400)
	if len(fragments) != 3 {
		t.Fatalf("fragment count = %d", len(fragments))
	}
	if len(fragments[0].payload) != 1400 || len(fragments[1].payload) != 1400 || len(fragments[2].payload) != 1 {
		t.Fatalf("fragment sizes = %d %d %d", len(fragments[0].payload), len(fragments[1].payload), len(fragments[2].payload))
	}
	if fragments[0].marker || fragments[1].marker || fragments[2].marker {
		t.Fatalf("markers = %t %t %t", fragments[0].marker, fragments[1].marker, fragments[2].marker)
	}
	payload[0] = 0xee
	if fragments[0].payload[0] != 0xee {
		t.Fatal("fragment copied the shared payload")
	}
}

func TestRTPSessionWritesIndependentHeaders(t *testing.T) {
	session := rtpSession{ssrc: 0x10203040, sequence: 0x1122, timestampOffset: 90}
	var first, second [12]byte
	session.writeHeader(first[:], 96, 3600, false)
	session.writeHeader(second[:], 96, 3600, true)
	if !bytes.Equal(first[:2], []byte{0x80, 96}) || !bytes.Equal(second[:2], []byte{0x80, 0x80 | 96}) {
		t.Fatalf("RTP flags first=%x second=%x", first[:2], second[:2])
	}
	if binary.BigEndian.Uint16(first[2:4]) != 0x1122 || binary.BigEndian.Uint16(second[2:4]) != 0x1123 {
		t.Fatalf("RTP sequences first=%x second=%x", first[2:4], second[2:4])
	}
	if binary.BigEndian.Uint32(first[4:8]) != 3690 || binary.BigEndian.Uint32(second[4:8]) != 3690 {
		t.Fatalf("RTP timestamps first=%d second=%d", binary.BigEndian.Uint32(first[4:8]), binary.BigEndian.Uint32(second[4:8]))
	}
	if binary.BigEndian.Uint32(first[8:12]) != 0x10203040 || binary.BigEndian.Uint32(second[8:12]) != 0x10203040 {
		t.Fatalf("RTP SSRC first=%x second=%x", first[8:12], second[8:12])
	}
	if session.sequence != 0x1124 {
		t.Fatalf("next sequence = %x", session.sequence)
	}
}
