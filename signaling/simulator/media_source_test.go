package main

import (
	"bytes"
	"testing"
)

var testAnnexB = []byte{
	0, 0, 0, 1, 0x09, 0xf0,
	0, 0, 0, 1, 0x67, 0x42, 0x00, 0x1e,
	0, 0, 0, 1, 0x68, 0xce, 0x06, 0xe2,
	0, 0, 0, 1, 0x65, 0x88, 0x84,
	0, 0, 0, 1, 0x09, 0xf0,
	0, 0, 0, 1, 0x41, 0x9a, 0x22,
}

func TestParseAccessUnitsPreservesFramesAndKeyframes(t *testing.T) {
	units, err := parseAccessUnits(testAnnexB)
	if err != nil {
		t.Fatalf("parseAccessUnits() error = %v", err)
	}
	if len(units) != 2 {
		t.Fatalf("unit count = %d", len(units))
	}
	if !units[0].keyframe || units[1].keyframe {
		t.Fatalf("keyframes = %t %t", units[0].keyframe, units[1].keyframe)
	}
	if !bytes.Equal(units[0].annexB, testAnnexB[6:29]) || !bytes.Equal(units[1].annexB, testAnnexB[35:]) {
		t.Fatalf("access unit boundaries are wrong: first=%x second=%x", units[0].annexB, units[1].annexB)
	}
}

func TestParseAccessUnitsRejectsMissingConfigurationOrAUD(t *testing.T) {
	for name, data := range map[string][]byte{
		"no AUD":    {0, 0, 0, 1, 0x67, 1, 0, 0, 0, 1, 0x68, 1, 0, 0, 0, 1, 0x65, 1},
		"no config": {0, 0, 0, 1, 0x09, 0xf0, 0, 0, 0, 1, 0x65, 1},
	} {
		t.Run(name, func(t *testing.T) {
			if _, err := parseAccessUnits(data); err == nil {
				t.Fatal("parseAccessUnits() succeeded")
			}
		})
	}
}

func TestSharedMediaSourceTimelineContinuesAcrossLoop(t *testing.T) {
	source, err := newSharedMediaSource(testAnnexB)
	if err != nil {
		t.Fatalf("newSharedMediaSource() error = %v", err)
	}
	wantTimestamps := []uint32{3600, 7200, 10800, 14400}
	wantKeyframes := []bool{true, false, true, false}
	for index := range wantTimestamps {
		unit, err := source.next()
		if err != nil {
			t.Fatalf("next(%d) error = %v", index, err)
		}
		if unit.timestamp != wantTimestamps[index] || unit.keyframe != wantKeyframes[index] {
			t.Fatalf("unit %d timestamp=%d keyframe=%t", index, unit.timestamp, unit.keyframe)
		}
		if len(unit.payload) < 4 || !bytes.Equal(unit.payload[:4], []byte{0, 0, 1, 0xba}) {
			t.Fatalf("unit %d is not MPEG-PS: %x", index, unit.payload)
		}
		if unit.keyframe && !bytes.Contains(unit.payload, []byte{0, 0, 1, 0xbc}) {
			t.Fatalf("keyframe unit %d has no program stream map", index)
		}
	}
}
