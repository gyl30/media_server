package main

import "encoding/binary"

const maxRTPPayload = 1400

type rtpFragment struct {
	payload []byte
	marker  bool
}

func fragmentPSPayload(payload []byte, maximum int) []rtpFragment {
	fragments := make([]rtpFragment, 0, (len(payload)+maximum-1)/maximum)
	for len(payload) > maximum {
		fragments = append(fragments, rtpFragment{payload: payload[:maximum]})
		payload = payload[maximum:]
	}
	fragments = append(fragments, rtpFragment{payload: payload})
	return fragments
}

type rtpSession struct {
	ssrc            uint32
	sequence        uint16
	timestampOffset uint32
}

func (s *rtpSession) writeHeader(header []byte, payloadType uint8, timestamp uint32, marker bool) {
	header[0] = 0x80
	header[1] = payloadType
	if marker {
		header[1] |= 0x80
	}
	binary.BigEndian.PutUint16(header[2:4], s.sequence)
	binary.BigEndian.PutUint32(header[4:8], timestamp+s.timestampOffset)
	binary.BigEndian.PutUint32(header[8:12], s.ssrc)
	s.sequence++
}
