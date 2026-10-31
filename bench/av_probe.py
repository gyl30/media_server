#!/usr/bin/env python3
"""Decode one live output with ffmpeg and report frame cadence, timestamp order, arrival lag and A/V marker offsets."""

import argparse
import json
import re
import statistics
import subprocess
import time

FILTER_LINE = re.compile(r"^\[Parsed_(\w+?_\d+) @ [^\]]+\] (.*)")
FRAME_HEADER = re.compile(r"^frame:\d+\s+pts:\S+\s+pts_time:(-?[\d.]+)")
METADATA = re.compile(r"^lavfi\.(signalstats\.YAVG|astats\.Overall\.Number_of_samples|astats\.Overall\.RMS_level)=(\S+)")
AUDIO_RATE = re.compile(r"Stream #0:\d+.*: Audio: .*?, (\d+) Hz")
DECODE_WARNING = re.compile(r"error|corrupt|concealing|non-monoton|missing picture|invalid", re.IGNORECASE)
FLASH_LUMA = 170
BEEP_DB = -30.0
STALL_SECONDS = 0.3
AUDIO_GAP_SECONDS = 0.005


def rising_edges(samples, threshold):
    edges = []
    above = False
    for pts, value in samples:
        if value >= threshold and not above:
            edges.append(pts)
        above = value >= threshold
    return edges


def summarize(video, audio_frames, audio_levels, audio_rate, first_video_s, first_audio_s, decode_warnings, exit_code, seconds):
    result = {"exit_code": exit_code, "seconds": seconds, "first_video_s": first_video_s, "first_audio_s": first_audio_s,
              "decode_warnings": decode_warnings, "video_frames": len(video), "audio_frames": len(audio_frames)}
    if video and audio_frames:
        result["audio_start_after_video_ms"] = (audio_frames[0][0] - video[0][0]) * 1000
    if len(video) >= 2:
        pts = [frame[0] for frame in video]
        intervals = [later - earlier for earlier, later in zip(pts, pts[1:])]
        nominal = statistics.median([value for value in intervals if value > 0] or [0])
        arrivals = [later[2] - earlier[2] for earlier, later in zip(video, video[1:])]
        lags = [wall - (frame_pts - pts[0]) for frame_pts, _, wall in video]
        span = pts[-1] - pts[0]
        result.update({
            "video_fps": (len(pts) - 1) / span if span > 0 else 0,
            "video_pts_backward": sum(value <= 0 for value in intervals),
            "video_pts_gaps": sum(value > nominal * 2.5 for value in intervals),
            "video_pts_gap_max_ms": max(intervals) * 1000,
            "video_arrival_stalls": sum(value > STALL_SECONDS for value in arrivals),
            "video_arrival_gap_max_ms": max(arrivals) * 1000,
            "lag_span_ms": (max(lags) - min(lags)) * 1000,
            "lag_end_ms": (lags[-1] - lags[0]) * 1000,
        })
    if len(audio_frames) >= 2 and audio_rate:
        slips = [later[0] - (earlier[0] + earlier[1] / audio_rate) for earlier, later in zip(audio_frames, audio_frames[1:])]
        result.update({
            "audio_rate": audio_rate,
            "audio_pts_overlaps": sum(value < -AUDIO_GAP_SECONDS for value in slips),
            "audio_pts_gaps": sum(value > AUDIO_GAP_SECONDS for value in slips),
            "audio_pts_gap_max_ms": max(slips) * 1000,
        })
    flashes = rising_edges([(frame[0], frame[1]) for frame in video], FLASH_LUMA)
    beeps = rising_edges(audio_levels, BEEP_DB)
    offsets = []
    for flash in flashes:
        nearest = min(beeps, key=lambda beep: abs(beep - flash), default=None)
        if nearest is not None and abs(nearest - flash) < 0.5:
            offsets.append((flash - nearest) * 1000)
    result.update({"av_markers": len(offsets), "av_offset_ms_min": min(offsets, default=None),
                   "av_offset_ms_p50": statistics.median(offsets) if offsets else None, "av_offset_ms_max": max(offsets, default=None)})
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("url")
    parser.add_argument("--seconds", type=float, default=20)
    parser.add_argument("--ffmpeg-bin", default="ffmpeg")
    args = parser.parse_args()

    audio_chain = ("astats=metadata=1:reset=1:measure_perchannel=none:measure_overall=Number_of_samples,"
                   "ametadata=mode=print:key=lavfi.astats.Overall.Number_of_samples,asetnsamples=n=160:p=0,"
                   "astats=metadata=1:reset=1:measure_perchannel=none:measure_overall=RMS_level,"
                   "ametadata=mode=print:key=lavfi.astats.Overall.RMS_level")
    command = [args.ffmpeg_bin, "-hide_banner", "-nostats", "-loglevel", "info"]
    command += ["-rtsp_transport", "tcp"] if args.url.startswith("rtsp://") else []
    command += ["-i", args.url, "-t", str(args.seconds), "-map", "0:v:0", "-map", "0:a:0?",
                "-filter:v", "scale=64:36,signalstats,metadata=mode=print:key=lavfi.signalstats.YAVG", "-filter:a", audio_chain,
                "-fps_mode", "passthrough", "-f", "null", "-"]
    started = time.monotonic()
    video = []
    audio_frames = []
    audio_levels = []
    audio_rate = None
    pending = {}
    first_video_s = None
    first_audio_s = None
    decode_warnings = 0
    process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True, errors="replace")
    for line in process.stderr or []:
        now = time.monotonic() - started
        filtered = FILTER_LINE.match(line)
        if filtered is None:
            if match := AUDIO_RATE.search(line):
                audio_rate = audio_rate or int(match.group(1))
            elif DECODE_WARNING.search(line):
                decode_warnings += 1
            continue
        name, text = filtered.groups()
        if match := FRAME_HEADER.match(text):
            pending[name] = float(match.group(1))
            continue
        match = METADATA.match(text)
        if match is None or name not in pending:
            continue
        pts = pending.pop(name)
        key, value = match.groups()
        if key == "signalstats.YAVG":
            if first_video_s is None:
                first_video_s = now
            video.append((pts, float(value), now - first_video_s))
        elif key == "astats.Overall.Number_of_samples":
            if first_audio_s is None:
                first_audio_s = now
            audio_frames.append((pts, float(value)))
        else:
            audio_levels.append((pts, float(value) if value not in ("-inf", "inf", "nan") else -200.0))
    process.wait()
    print(json.dumps(summarize(video, audio_frames, audio_levels, audio_rate, first_video_s, first_audio_s, decode_warnings, process.returncode, args.seconds)))


if __name__ == "__main__":
    main()
