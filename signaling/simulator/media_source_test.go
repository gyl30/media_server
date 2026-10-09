package main

import (
	"bytes"
	"os"
	"path/filepath"
	"testing"

	mpeg2 "github.com/yapingcat/gomedia/go-mpeg2"
)

func testH264AccessUnits() []byte {
	return []byte{
		0, 0, 0, 1, 9, 0xf0,
		0, 0, 0, 1, 0x67, 0x42, 0, 0x1f,
		0, 0, 0, 1, 0x68, 0xce, 0x3c,
		0, 0, 0, 1, 0x65, 0x88, 0x84,
		0, 0, 0, 1, 9, 0xf0,
		0, 0, 0, 1, 0x41, 0x9a, 0x20,
	}
}

func TestSharedMediaSourceVideoOnly(t *testing.T) {
	source, err := newSharedMediaSource(testH264AccessUnits(), nil)
	if err != nil {
		t.Fatal(err)
	}
	demuxer := mpeg2.NewPSDemuxer()
	maps, videoPES := 0, 0
	demuxer.OnPacket = func(packet mpeg2.Display, err error) {
		if err != nil {
			t.Fatal(err)
		}
		switch packet := packet.(type) {
		case *mpeg2.Program_stream_map:
			maps++
			if len(packet.Stream_map) != 1 || packet.Stream_map[0].Stream_type != 0x1b || packet.Stream_map[0].Elementary_stream_id != 0xe0 {
				t.Fatalf("unexpected video-only PSM: %+v", packet.Stream_map)
			}
		case *mpeg2.PesPacket:
			if packet.Stream_id != 0xe0 || packet.Pts != 3600 || packet.Dts != 3600 {
				t.Fatalf("unexpected video PES: %+v", packet)
			}
			videoPES++
		}
	}
	unit, err := source.next()
	if err != nil {
		t.Fatal(err)
	}
	if !unit.keyframe || unit.timestamp != 3600 {
		t.Fatalf("unexpected first video unit: %+v", unit)
	}
	var payload []byte
	for _, fragment := range unit.fragments {
		payload = append(payload, fragment.payload...)
	}
	if err := demuxer.Input(payload); err != nil {
		t.Fatal(err)
	}
	if maps != 1 || videoPES != 1 {
		t.Fatalf("PSM=%d video PES=%d", maps, videoPES)
	}
}

func TestSharedMediaSourcePCMATimingAndLoop(t *testing.T) {
	audio := append(bytes.Repeat([]byte{0xd5}, 320), bytes.Repeat([]byte{0x55}, 320)...)
	source, err := newSharedMediaSource(testH264AccessUnits(), audio)
	if err != nil {
		t.Fatal(err)
	}
	demuxer := mpeg2.NewPSDemuxer()
	frame, maps, videoPES, audioPES, audioFrames := 0, 0, 0, 0, 0
	demuxer.OnPacket = func(packet mpeg2.Display, err error) {
		if err != nil {
			t.Fatal(err)
		}
		switch packet := packet.(type) {
		case *mpeg2.Program_stream_map:
			maps++
			if len(packet.Stream_map) != 2 || packet.Stream_map[0].Stream_type != 0x1b || packet.Stream_map[0].Elementary_stream_id != 0xe0 ||
				packet.Stream_map[1].Stream_type != 0x90 || packet.Stream_map[1].Elementary_stream_id != 0xc0 {
				t.Fatalf("unexpected AV PSM: %+v", packet.Stream_map)
			}
		case *mpeg2.PSPackHeader:
			if packet.System_clock_reference_base != uint64(frame)*3600 {
				t.Fatalf("frame %d SCR=%d", frame, packet.System_clock_reference_base)
			}
		case *mpeg2.PesPacket:
			if maps == 0 || packet.Pts != uint64(frame+1)*3600 || packet.Dts != packet.Pts {
				t.Fatalf("frame %d PSM=%d PES=%+v", frame, maps, packet)
			}
			switch packet.Stream_id {
			case 0xe0:
				videoPES++
			case 0xc0:
				if videoPES != audioPES+1 || !bytes.Equal(packet.Pes_payload, audio[(frame%2)*320:(frame%2+1)*320]) {
					t.Fatalf("frame %d unexpected PCMA payload/order", frame)
				}
				audioPES++
			default:
				t.Fatalf("unexpected PES stream %x", packet.Stream_id)
			}
		}
	}
	demuxer.OnFrame = func(payload []byte, codec mpeg2.PS_STREAM_TYPE, pts, dts uint64) {
		if codec != mpeg2.PS_STREAM_G711A {
			return
		}
		if pts != uint64(audioFrames+1)*40 || dts != pts || !bytes.Equal(payload, audio[(audioFrames%2)*320:(audioFrames%2+1)*320]) {
			t.Fatalf("audio frame %d pts=%d dts=%d bytes=%d", audioFrames, pts, dts, len(payload))
		}
		audioFrames++
	}
	var firstUnit mediaUnit
	var firstPayload []byte
	for frame = 0; frame < 6; frame++ {
		unit, err := source.next()
		if err != nil {
			t.Fatal(err)
		}
		if unit.timestamp != uint32(frame+1)*3600 || unit.keyframe != (frame%2 == 0) {
			t.Fatalf("frame %d timestamp=%d keyframe=%v", frame, unit.timestamp, unit.keyframe)
		}
		var payload []byte
		for _, fragment := range unit.fragments {
			if len(fragment.payload) == 0 || len(fragment.payload) > maxRTPPayload {
				t.Fatalf("unexpected RTP fragment length %d", len(fragment.payload))
			}
			payload = append(payload, fragment.payload...)
		}
		if frame == 0 {
			firstUnit, firstPayload = unit, bytes.Clone(payload)
		}
		if err := demuxer.Input(payload); err != nil {
			t.Fatal(err)
		}
	}
	demuxer.Flush()
	if maps != 3 || videoPES != 6 || audioPES != 6 || audioFrames != 6 {
		t.Fatalf("PSM=%d video PES=%d audio PES=%d audio frames=%d", maps, videoPES, audioPES, audioFrames)
	}
	var retainedPayload []byte
	for _, fragment := range firstUnit.fragments {
		retainedPayload = append(retainedPayload, fragment.payload...)
	}
	if !bytes.Equal(firstPayload, retainedPayload) {
		t.Fatal("later source units overwrote retained RTP payload")
	}
}

func TestSharedMediaSourceRejectsInvalidPCMA(t *testing.T) {
	for _, size := range []int{0, 1, 319, 321} {
		if _, err := newSharedMediaSource(testH264AccessUnits(), make([]byte, size)); err == nil {
			t.Fatalf("accepted %d-byte PCMA input", size)
		}
	}
}

func TestLoadSharedMediaSourcePCMA(t *testing.T) {
	directory := t.TempDir()
	videoPath, audioPath := filepath.Join(directory, "video.h264"), filepath.Join(directory, "audio.alaw")
	if err := os.WriteFile(videoPath, testH264AccessUnits(), 0600); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(audioPath, bytes.Repeat([]byte{0xd5}, 320), 0600); err != nil {
		t.Fatal(err)
	}
	if _, err := loadSharedMediaSource(videoPath, audioPath); err != nil {
		t.Fatal(err)
	}
	if _, err := loadSharedMediaSource(videoPath, ""); err != nil {
		t.Fatal(err)
	}
	if _, err := loadSharedMediaSource(videoPath, filepath.Join(directory, "missing.alaw")); err == nil {
		t.Fatal("accepted missing PCMA file")
	}
	if err := os.WriteFile(audioPath, nil, 0600); err != nil {
		t.Fatal(err)
	}
	if _, err := loadSharedMediaSource(videoPath, audioPath); err == nil {
		t.Fatal("accepted empty PCMA file")
	}
}
