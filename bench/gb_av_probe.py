#!/usr/bin/env python3
"""Check a GB28181 RTP/PS dump end to end: gb_ps_demux splits it into elementary streams with their PS timestamps, ffmpeg decodes each stream
as its known codec, and frames, samples, flashes and beeps are placed on the PS PTS timeline rather than on ffmpeg's generated timestamps.
With --reference the audio elementary stream is also compared, frame by frame or byte by byte, with the source fixture's audio."""

import argparse
import array
import bisect
import csv
import json
import math
import re
import statistics
import subprocess
import sys
from pathlib import Path

VIDEO_FORMATS = {"h264": "h264", "hevc": "hevc"}
YAVG = re.compile(r"lavfi\.signalstats\.YAVG=(\S+)")
DECODE_WARNING = re.compile(r"error|corrupt|concealing|non-monoton|missing picture|invalid", re.IGNORECASE)
ADTS_RATES = (96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350)
CLOCK = 90000
FLASH_LUMA = 170
BEEP_DB = -30.0
AUDIO_GAP_SECONDS = 0.005
REFERENCE_FORMATS = {"aac": "adts", "alaw": "alaw", "mulaw": "mulaw"}
ANCHOR_UNITS = 100


def rising_edges(samples, threshold):
    edges = []
    above = False
    for pts, value in samples:
        if value >= threshold and not above:
            edges.append(pts)
        above = value >= threshold
    return edges


def adts_frames(data):
    frames = []
    offset = 0
    while offset + 7 <= len(data):
        length = ((data[offset + 3] & 3) << 11) | (data[offset + 4] << 3) | (data[offset + 5] >> 5)
        header = 7 if data[offset + 1] & 1 else 9
        frames.append(data[offset + header:offset + length])
        offset += length
    return frames


def decode_video(args, codec, path):
    command = [args.ffmpeg_bin, "-hide_banner", "-nostats", "-loglevel", "info", "-f", VIDEO_FORMATS[codec], "-i", str(path),
               "-vf", "scale=64:36,signalstats,metadata=mode=print:key=lavfi.signalstats.YAVG", "-fps_mode", "passthrough", "-f", "null", "-"]
    process = subprocess.run(command, capture_output=True, text=True, errors="replace")
    luma = []
    warnings = 0
    for line in process.stderr.splitlines():
        if match := YAVG.search(line):
            luma.append(float(match.group(1)))
        elif DECODE_WARNING.search(line) and str(path) not in line:
            warnings += 1
    return process.returncode, luma, warnings


def decode_audio(args, codec, path):
    command = [args.ffmpeg_bin, "-hide_banner", "-nostats", "-loglevel", "warning", "-f", codec]
    command += ["-ar", "8000", "-ac", "1"] if codec in ("alaw", "mulaw") else []
    command += ["-i", str(path), "-ac", "1", "-f", "s16le", "-"]
    process = subprocess.run(command, capture_output=True)
    samples = array.array("h")
    samples.frombytes(process.stdout[:len(process.stdout) // 2 * 2])
    if sys.byteorder == "big":
        samples.byteswap()
    stderr = process.stderr.decode(errors="replace")
    return process.returncode, samples, sum(bool(DECODE_WARNING.search(line)) and str(path) not in line for line in stderr.splitlines())


def compare_reference(args, codec, es, rows):
    if codec not in REFERENCE_FORMATS:
        return {"status": "unsupported codec"}
    path = args.output_prefix.with_name(args.output_prefix.name + ".reference")
    process = subprocess.run([args.ffmpeg_bin, "-hide_banner", "-loglevel", "error", "-y", "-i", str(args.reference), "-map", "0:a:0", "-c:a", "copy",
                              "-f", REFERENCE_FORMATS[codec], str(path)], capture_output=True, text=True)
    if process.returncode != 0:
        return {"status": "reference extraction failed", "error": process.stderr[-300:]}
    reference = path.read_bytes()
    if codec == "aac":
        units = adts_frames(es)
        known = adts_frames(reference)
        size = len(known)
        starts = {}
        for index, unit in enumerate(known):
            starts.setdefault(unit, []).append(index)

        def candidates(first):
            return starts.get(units[first], [])

        def window_matches(position, first, count):
            return all(known[(position + offset) % size] == unit for offset, unit in enumerate(units[first:first + count]))

        def matches(position, unit):
            return known[position] == unit

        def step(unit):
            return 1
    else:
        units = [es[int(row["offset"]):int(row["offset"]) + int(row["bytes"])] for row in rows]
        size = len(reference)
        ring = reference * (2 + len(es) // max(size, 1))

        def candidates(first):
            found = []
            position = ring.find(units[first]) if units[first] else -1
            while 0 <= position < size:
                found.append(position)
                position = ring.find(units[first], position + 1)
            return found

        def window_matches(position, first, count):
            joined = b"".join(units[first:first + count])
            return ring[position:position + len(joined)] == joined

        def matches(position, unit):
            return ring[position:position + len(unit)] == unit

        def step(unit):
            return len(unit)

    def locate(first):
        count = ANCHOR_UNITS
        found = [position for position in candidates(first) if window_matches(position, first, count)]
        while len(found) > 1 and first + count < len(units):
            count *= 2
            found = [position for position in found if window_matches(position, first, count)]
        return found[0] if found else None

    result = {"status": "compared", "units": len(units), "matched": 0, "unmatched": 0, "jump_count": 0, "jumps": []}
    anchor = locate(0) if units and size else None
    if anchor is None:
        result.update({"status": "the first units do not occur in the reference", "ok": False})
        return result
    position = anchor
    for index, unit in enumerate(units):
        if not matches(position, unit):
            found = locate(index)
            if found is None:
                result["unmatched"] += 1
                position = (position + step(unit)) % size
                continue
            result["jump_count"] += 1
            if len(result["jumps"]) < 20:
                result["jumps"].append({"unit": index, "expected": position, "found": found})
            position = found
        result["matched"] += 1
        position = (position + step(unit)) % size
    result["ok"] = result["unmatched"] == 0 and result["jump_count"] == 0 and result["matched"] > 0
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dump", type=Path)
    parser.add_argument("--demux-bin", type=Path, required=True)
    parser.add_argument("--ffmpeg-bin", default="ffmpeg")
    parser.add_argument("--output-prefix", type=Path, help="default: the dump path without its suffix")
    parser.add_argument("--payload-type", type=int, default=96)
    parser.add_argument("--expect-audio", action="store_true")
    parser.add_argument("--reference", type=Path, help="source fixture whose audio the output must reproduce unchanged")
    args = parser.parse_args()
    args.output_prefix = args.output_prefix or args.dump.with_suffix("")

    demux = subprocess.run([str(args.demux_bin), "--input", str(args.dump), "--output-prefix", str(args.output_prefix), "--payload-type",
                            str(args.payload_type)], capture_output=True, text=True)
    try:
        summary = json.loads(demux.stdout)
    except ValueError:
        summary = {"error_count": 1, "errors": [demux.stderr[-300:]]}
    result = {"demux_exit": demux.returncode, "demux": summary, "video_codec": summary.get("video_codec"), "audio_codec": summary.get("audio_codec")}
    rows = list(csv.DictReader(open(f"{args.output_prefix}.csv"), fieldnames=("kind", "pts", "dts", "key", "offset", "bytes", "units"))) \
        if Path(f"{args.output_prefix}.csv").exists() else []
    video_rows = [row for row in rows if row["kind"] == "video"]
    audio_rows = [row for row in rows if row["kind"] == "audio"]

    flashes = []
    if result["video_codec"] in VIDEO_FORMATS:
        code, luma, warnings = decode_video(args, result["video_codec"], Path(f"{args.output_prefix}.{result['video_codec']}"))
        pts = sorted(int(row["pts"]) for row in video_rows)
        intervals = [later - earlier for earlier, later in zip(pts, pts[1:])]
        nominal = statistics.median([value for value in intervals if value > 0] or [0])
        result.update({"video_decode_exit": code, "video_frames": len(video_rows), "video_decoded_frames": len(luma), "decode_warnings": warnings,
                       "video_pts_backward": sum(value <= 0 for value in intervals), "video_pts_gaps": sum(value > nominal * 2.5 for value in intervals),
                       "video_fps": (len(pts) - 1) * CLOCK / (pts[-1] - pts[0]) if len(pts) > 1 and pts[-1] > pts[0] else 0})
        if len(luma) == len(pts):
            flashes = rising_edges([(value / CLOCK, level) for value, level in zip(pts, luma)], FLASH_LUMA)

    beeps = []
    if result["audio_codec"]:
        es_path = Path(f"{args.output_prefix}.{result['audio_codec']}")
        es = es_path.read_bytes()
        rate = 8000
        per_unit = 1
        if result["audio_codec"] == "aac":
            rate = ADTS_RATES[(es[2] >> 2) & 0x0F] if len(es) > 2 else 0
            per_unit = 1024
        code, samples, warnings = decode_audio(args, result["audio_codec"], es_path)
        counts = [int(row["units"]) * per_unit for row in audio_rows]
        starts = [0]
        for count in counts:
            starts.append(starts[-1] + count)
        pts = [int(row["pts"]) / CLOCK for row in audio_rows]
        slips = [later - (earlier + count / rate) for earlier, later, count in zip(pts, pts[1:], counts)] if rate else []
        window = max(1, rate // 100)
        levels = []
        for start in range(0, len(samples) - window + 1, window):
            frame = samples[start:start + window]
            rms = math.sqrt(sum(value * value for value in frame) / window)
            position = bisect.bisect_right(starts, start) - 1
            if 0 <= position < len(pts):
                levels.append((pts[position] + (start - starts[position]) / rate, 20 * math.log10(rms / 32768) if rms > 0 else -200.0))
        beeps = rising_edges(levels, BEEP_DB)
        overall = math.sqrt(sum(value * value for value in samples) / len(samples)) if samples else 0
        result.update({"audio_decode_exit": code, "audio_frames": len(audio_rows), "audio_rate": rate, "audio_samples": len(samples),
                       "audio_expected_samples": starts[-1], "audio_decode_warnings": warnings,
                       "audio_rms_db": round(20 * math.log10(overall / 32768), 1) if overall > 0 else None,
                       "audio_silent_windows": sum(level < -60 for _, level in levels), "audio_windows": len(levels),
                       "audio_pts_gaps": sum(value > AUDIO_GAP_SECONDS for value in slips),
                       "audio_pts_overlaps": sum(value < -AUDIO_GAP_SECONDS for value in slips),
                       "audio_pts_slip_ms_max": round(max(slips, default=0) * 1000, 3), "audio_pts_slip_ms_min": round(min(slips, default=0) * 1000, 3),
                       "audio_pts_slips": [{"frame": index + 1, "ms": round(value * 1000, 3)} for index, value in enumerate(slips)
                                           if abs(value) > AUDIO_GAP_SECONDS][:20]})
        if args.reference:
            result["reference"] = compare_reference(args, result["audio_codec"], es, audio_rows)

    offsets = []
    for flash in flashes:
        nearest = min(beeps, key=lambda beep: abs(beep - flash), default=None)
        if nearest is not None and abs(nearest - flash) < 0.5:
            offsets.append((flash - nearest) * 1000)
    result.update({"av_markers": len(offsets), "av_offset_ms_min": min(offsets, default=None),
                   "av_offset_ms_p50": statistics.median(offsets) if offsets else None, "av_offset_ms_max": max(offsets, default=None)})
    checks = {
        "demux": demux.returncode == 0,
        "video_decoded": result.get("video_decode_exit") == 0 and result.get("video_frames", 0) > 0
        and result.get("video_decoded_frames") == result.get("video_frames") and result.get("decode_warnings") == 0,
        "video_pts_order": result.get("video_pts_backward") == 0,
        "audio_present": bool(result["audio_codec"]) or not args.expect_audio,
        "audio_decoded": not result["audio_codec"] or (result.get("audio_decode_exit") == 0 and result.get("audio_decode_warnings") == 0
                                                         and result.get("audio_samples") == result.get("audio_expected_samples") > 0),
        "audio_timeline": not result["audio_codec"] or (result.get("audio_pts_gaps") == 0 and result.get("audio_pts_overlaps") == 0),
        "av_markers": not result["audio_codec"] or result["av_markers"] > 0,
        "reference": "reference" not in result or result["reference"].get("ok", False),
    }
    result["checks"] = checks
    result["ok"] = all(checks.values())
    print(json.dumps(result))
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
