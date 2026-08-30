package main

import (
	"bytes"
	"context"
	"fmt"
	"os"
	"os/exec"
	"strconv"
	"time"
)

type mediaFixture struct {
	path    string
	cleanup func()
}

func prepareMediaFixture(ctx context.Context, cfg config) (mediaFixture, error) {
	if cfg.mediaFile != "" {
		if _, err := os.Stat(cfg.mediaFile); err != nil {
			return mediaFixture{}, fmt.Errorf("media file: %w", err)
		}
		return mediaFixture{path: cfg.mediaFile, cleanup: func() {}}, nil
	}
	file, err := os.CreateTemp("", "gb28181-simulator-*.h264")
	if err != nil {
		return mediaFixture{}, err
	}
	path := file.Name()
	if err := file.Close(); err != nil {
		_ = os.Remove(path)
		return mediaFixture{}, err
	}
	cleanup := func() { _ = os.Remove(path) }
	duration := cfg.liveDuration + 5*time.Second
	bitrate := "1100k"
	bufferSize := "2200k"
	if cfg.mediaProfile == "high" {
		bitrate = "1500k"
		bufferSize = "3000k"
	}
	command := exec.CommandContext(
		ctx,
		cfg.ffmpeg,
		"-hide_banner", "-loglevel", "error",
		"-f", "lavfi", "-i", "testsrc2=size=320x240:rate=25",
		"-t", strconv.FormatFloat(duration.Seconds(), 'f', 3, 64),
		"-c:v", "libx264", "-preset", "ultrafast", "-tune", "zerolatency",
		"-b:v", bitrate, "-minrate", bitrate, "-maxrate", bitrate, "-bufsize", bufferSize,
		"-x264-params", "nal-hrd=cbr:force-cfr=1",
		"-pix_fmt", "yuv420p", "-g", "25", "-keyint_min", "25", "-sc_threshold", "0", "-bf", "0",
		"-bsf:v", "h264_metadata=aud=insert", "-an", "-f", "h264", "-y", path,
	)
	output, err := command.CombinedOutput()
	if err != nil {
		cleanup()
		return mediaFixture{}, fmt.Errorf("generate H264 fixture: %w: %s", err, bytes.TrimSpace(output))
	}
	return mediaFixture{path: path, cleanup: cleanup}, nil
}

func loadSharedMediaSource(path string) (*sharedMediaSource, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, fmt.Errorf("read H264 media: %w", err)
	}
	return newSharedMediaSource(data)
}
