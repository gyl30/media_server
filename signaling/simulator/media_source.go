package main

import (
	"fmt"

	mpeg2 "github.com/yapingcat/gomedia/go-mpeg2"
)

const frameDurationMillis = uint64(40)
const pcmaPacketBytes = 320

type encodedAccessUnit struct {
	annexB   []byte
	keyframe bool
}

type mediaUnit struct {
	fragments []rtpFragment
	timestamp uint32
	keyframe  bool
}

type nalPosition struct {
	start  int
	header int
}

func parseAccessUnits(data []byte) ([]encodedAccessUnit, error) {
	positions := make([]nalPosition, 0, 64)
	for offset := 0; offset+3 < len(data); {
		prefix := 0
		if offset+4 < len(data) && data[offset] == 0 && data[offset+1] == 0 && data[offset+2] == 0 && data[offset+3] == 1 {
			prefix = 4
		} else if data[offset] == 0 && data[offset+1] == 0 && data[offset+2] == 1 {
			prefix = 3
		}
		if prefix == 0 {
			offset++
			continue
		}
		positions = append(positions, nalPosition{start: offset, header: offset + prefix})
		offset += prefix + 1
	}
	accessStarts := make([]int, 0, 64)
	hasSPS := false
	hasPPS := false
	for _, position := range positions {
		if position.header >= len(data) {
			return nil, fmt.Errorf("truncated Annex-B NAL")
		}
		switch data[position.header] & 0x1f {
		case 7:
			hasSPS = true
		case 8:
			hasPPS = true
		case 9:
			accessStarts = append(accessStarts, position.start)
		}
	}
	if !hasSPS || !hasPPS || len(accessStarts) == 0 || accessStarts[0] != 0 {
		return nil, fmt.Errorf("H264 input requires SPS/PPS and AUD-delimited access units")
	}
	units := make([]encodedAccessUnit, 0, len(accessStarts))
	for index, start := range accessStarts {
		end := len(data)
		if index+1 < len(accessStarts) {
			end = accessStarts[index+1]
		}
		payloadStart := end
		keyframe := false
		for _, position := range positions {
			if position.start >= end {
				break
			}
			if position.start <= start {
				continue
			}
			if payloadStart == end {
				payloadStart = position.start
			}
			if data[position.header]&0x1f == 5 {
				keyframe = true
			}
		}
		if payloadStart == end {
			return nil, fmt.Errorf("empty Annex-B access unit")
		}
		units = append(units, encodedAccessUnit{annexB: data[payloadStart:end], keyframe: keyframe})
	}
	return units, nil
}

type sharedMediaSource struct {
	units         []encodedAccessUnit
	audio         []byte
	muxer         *mpeg2.PSMuxer
	videoStreamID uint8
	audioStreamID uint8
	frames        uint64
}

func newSharedMediaSource(data, audio []byte) (*sharedMediaSource, error) {
	units, err := parseAccessUnits(data)
	if err != nil {
		return nil, err
	}
	if audio != nil && (len(audio) == 0 || len(audio)%pcmaPacketBytes != 0) {
		return nil, fmt.Errorf("PCMA input requires a nonempty multiple of %d bytes at 8000 Hz mono", pcmaPacketBytes)
	}
	muxer := mpeg2.NewPsMuxer()
	source := &sharedMediaSource{
		units: units, audio: audio, muxer: muxer, videoStreamID: muxer.AddStream(mpeg2.PS_STREAM_H264),
	}
	if len(audio) > 0 {
		source.audioStreamID = muxer.AddStream(mpeg2.PS_STREAM_G711A)
	}
	return source, nil
}

func (s *sharedMediaSource) next() (mediaUnit, error) {
	accessUnit := s.units[s.frames%uint64(len(s.units))]
	timestampMillis := (s.frames + 1) * frameDurationMillis
	var payload []byte
	s.muxer.OnPacket = func(packet []byte) {
		payload = append(payload, packet...)
	}
	if err := s.muxer.Write(s.videoStreamID, accessUnit.annexB, timestampMillis, timestampMillis); err != nil {
		return mediaUnit{}, err
	}
	if len(s.audio) > 0 {
		start := int(s.frames%uint64(len(s.audio)/pcmaPacketBytes)) * pcmaPacketBytes
		if err := s.muxer.Write(s.audioStreamID, s.audio[start:start+pcmaPacketBytes], timestampMillis, timestampMillis); err != nil {
			return mediaUnit{}, err
		}
	}
	if len(payload) == 0 {
		return mediaUnit{}, fmt.Errorf("MPEG-PS muxer produced no payload")
	}
	s.frames++
	return mediaUnit{
		fragments: fragmentPSPayload(payload, maxRTPPayload), timestamp: uint32(timestampMillis * 90), keyframe: accessUnit.keyframe,
	}, nil
}
