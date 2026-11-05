#!/usr/bin/env python3
"""Replay variants of one recorded whep_fanout Opus dump through bench/whep_dump_av.py and check its audio verdicts.

The variants keep every media packet of the source dump and only change what each case names: the first audio RTCP Sender Report moved
50 ms after or 1 ms before the first audio RTP packet, four consecutive audio packets removed, a +80 ms or -20 ms RTP timestamp step,
audio sequence numbers and timestamps offset so both wrap, the audio track removed, and the audio rewritten as G.711 A-law (PT 8,
8 kHz timestamps, 160 silent bytes per 20 ms packet). Removed packets, steps and wraps are placed ten seconds into the replay window.
legacy_ok is the verdict bench/perf_run.py derived from the tool's output before the audio criteria existed; --legacy runs a tool without
the new arguments and checks that verdict against the expected one, which shows what the old criterion missed."""

import argparse
import json
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

AUDIO_PAYLOAD_TYPES = (111, 8)


def read_dump(path):
    data = Path(path).read_bytes()
    records = []
    offset = 0
    while offset + 11 <= len(data):
        nanoseconds, rtcp, size = struct.unpack_from("<qBH", data, offset)
        offset += 11
        records.append([nanoseconds, rtcp, bytearray(data[offset:offset + size])])
        offset += size
    return records


def write_dump(path, records):
    with open(path, "wb") as stream:
        for nanoseconds, rtcp, packet in records:
            stream.write(struct.pack("<qBH", nanoseconds, rtcp, len(packet)))
            stream.write(packet)


def is_audio_rtp(record):
    return not record[1] and len(record[2]) >= 12 and (record[2][1] & 0x7F) in AUDIO_PAYLOAD_TYPES


def first_audio(records):
    index = next(position for position, record in enumerate(records) if is_audio_rtp(record))
    ssrc = struct.unpack_from("!I", records[index][2], 8)[0]
    report = next(position for position, record in enumerate(records)
                  if record[1] and len(record[2]) >= 28 and record[2][1] == 200 and struct.unpack_from("!I", record[2], 4)[0] == ssrc)
    return index, report


def audio_at(records, seconds):
    start = records[0][0]
    return next(position for position, record in enumerate(records) if is_audio_rtp(record) and record[0] - start >= seconds * 1e9)


def move_report(records, offset_ns):
    records = [[record[0], record[1], bytearray(record[2])] for record in records]
    index, report = first_audio(records)
    moved = records.pop(report)
    moved[0] = records[index][0] + offset_ns
    position = next((at for at, record in enumerate(records) if record[0] > moved[0]), len(records))
    records.insert(position, moved)
    return records


def drop_audio(records, count):
    records = [[record[0], record[1], bytearray(record[2])] for record in records]
    start = audio_at(records, 10)
    removed = 0
    position = start
    while removed < count:
        if is_audio_rtp(records[position]):
            records.pop(position)
            removed += 1
        else:
            position += 1
    return records


def step_timestamps(records, ticks):
    records = [[record[0], record[1], bytearray(record[2])] for record in records]
    for position in range(audio_at(records, 10), len(records)):
        if is_audio_rtp(records[position]):
            timestamp = (struct.unpack_from("!I", records[position][2], 4)[0] + ticks) & 0xFFFFFFFF
            struct.pack_into("!I", records[position][2], 4, timestamp)
    return records


def audio_ssrc(records):
    return struct.unpack_from("!I", records[next(position for position, record in enumerate(records) if is_audio_rtp(record))][2], 8)[0]


def wrap_audio(records):
    records = [[record[0], record[1], bytearray(record[2])] for record in records]
    at = audio_at(records, 10)
    sequence, timestamp = struct.unpack_from("!HI", records[at][2], 2)
    for record in records:
        if is_audio_rtp(record):
            old_sequence, old_timestamp = struct.unpack_from("!HI", record[2], 2)
            struct.pack_into("!HI", record[2], 2, (old_sequence - sequence) & 0xFFFF, (old_timestamp - timestamp) & 0xFFFFFFFF)
    return records


def without_audio(records):
    ssrc = audio_ssrc(records)
    return [[record[0], record[1], bytearray(record[2])] for record in records
            if not (len(record[2]) >= 12 and struct.unpack_from("!I", record[2], 4 if record[1] else 8)[0] == ssrc)]


def as_pcma(records):
    ssrc = audio_ssrc(records)
    first = struct.unpack_from("!I", records[next(position for position, record in enumerate(records) if is_audio_rtp(record))][2], 4)[0]
    converted = []
    for nanoseconds, rtcp, packet in records:
        if not rtcp and is_audio_rtp([nanoseconds, rtcp, packet]):
            timestamp = (((struct.unpack_from("!I", packet, 4)[0] - first) & 0xFFFFFFFF) // 6 + first) & 0xFFFFFFFF
            packet = bytearray(packet[:12]) + b"\xd5" * 160
            packet[0] &= 0xC0
            packet[1] = (packet[1] & 0x80) | 8
            struct.pack_into("!I", packet, 4, timestamp)
        elif rtcp and len(packet) >= 20 and packet[1] == 200 and struct.unpack_from("!I", packet, 4)[0] == ssrc:
            continue
        converted.append([nanoseconds, rtcp, bytearray(packet)])
    return converted


EXPECT = ["--expect-audio"]
CASES = [
    ("original", lambda records: records, EXPECT, {"verdict": "PASS", "audio_pts_gaps": 0, "audio_pts_overlaps": 0, "audio_rtp_lost": 0}),
    ("sr-after-first-audio-50ms", lambda records: move_report(records, 50_000_000), EXPECT, {"verdict": "PASS", "audio_pts_gaps": 0, "recorded_audio_pts_gaps": 1}),
    ("sr-before-first-audio-1ms", lambda records: move_report(records, -1_000_000), EXPECT, {"verdict": "PASS", "audio_pts_gaps": 0}),
    ("drop-4-audio", lambda records: drop_audio(records, 4), EXPECT, {"verdict": "FAIL", "audio_pts_gaps": 1, "audio_rtp_lost": 4, "gap_ms": 80}),
    ("timestamp-step-plus-80ms", lambda records: step_timestamps(records, 3840), EXPECT, {"verdict": "FAIL", "audio_pts_gaps": 1, "audio_rtp_lost": 0, "gap_ms": 80}),
    ("timestamp-step-minus-20ms", lambda records: step_timestamps(records, -960), EXPECT, {"verdict": "FAIL", "audio_pts_overlaps": 1, "audio_rtp_lost": 0}),
    ("sequence-timestamp-wrap", wrap_audio, EXPECT, {"verdict": "PASS", "audio_pts_gaps": 0, "audio_pts_overlaps": 0, "audio_rtp_lost": 0}),
    ("wrap-then-drop-4-audio", lambda records: drop_audio(wrap_audio(records), 4), EXPECT, {"verdict": "FAIL", "audio_rtp_lost": 4, "gap_ms": 80}),
    ("pcma", as_pcma, EXPECT, {"verdict": "PASS", "audio_pts_gaps": 0, "audio_pts_overlaps": 0, "audio_rtp_lost": 0}),
    ("pcma-drop-4-audio", lambda records: drop_audio(as_pcma(records), 4), EXPECT, {"verdict": "FAIL", "audio_rtp_lost": 4, "gap_ms": 80}),
    ("audio-expected-but-absent", without_audio, EXPECT, {"verdict": "FAIL", "audio_rtp_packets": 0}),
    ("video-only-or-audio-rejected", without_audio, [], {"verdict": "PASS", "audio_rtp_packets": 0}),
    ("loop-seam-after-premise", lambda records: step_timestamps(records, 3840), EXPECT + ["--source-continuous-ns", "5000000000"],
     {"verdict": "PASS", "audio_pts_gaps": 0}),
    ("premise-before-first-audio", lambda records: records, EXPECT + ["--source-continuous-ns", "0"], {"verdict": "INCONCLUSIVE"}),
]


def legacy_ok(result):
    return result.get("exit_code") == 0 and result.get("video_frames", 0) > 0 and result.get("video_pts_backward") == 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dump")
    parser.add_argument("--tool", type=Path, default=Path(__file__).resolve().parents[3] / "bench" / "whep_dump_av.py")
    parser.add_argument("--ffmpeg-bin", default="ffmpeg")
    parser.add_argument("--seconds", type=float, default=25)
    parser.add_argument("--only", nargs="*")
    parser.add_argument("--legacy", action="store_true", help="judge with the pre-audio perf_run criterion and pass no new tool arguments")
    args = parser.parse_args()

    source = read_dump(args.dump)
    failures = 0
    with tempfile.TemporaryDirectory() as directory:
        for name, build, extra, expected in CASES:
            if args.only and name not in args.only:
                continue
            dump = Path(directory) / f"{name}.dump"
            write_dump(dump, build(source))
            completed = subprocess.run([sys.executable, str(args.tool), str(dump), "--seconds", str(args.seconds), "--output", str(dump.with_suffix(".mkv")),
                                        "--ffmpeg-bin", args.ffmpeg_bin, *([] if args.legacy else extra)], capture_output=True, text=True)
            lines = completed.stdout.strip().splitlines()
            result = json.loads(lines[-1]) if lines else {"error": completed.stderr[-300:]}
            if args.legacy:
                result["verdict"] = "PASS" if completed.returncode == 0 and legacy_ok(result) else "FAIL"
                expected = {"verdict": expected["verdict"]}
            if not args.legacy and (result.get("verdict") == "FAIL" and completed.returncode != 1 or result.get("verdict") in ("PASS", "INCONCLUSIVE") and completed.returncode != 0):
                expected = {**expected, "exit": completed.returncode}
                result["exit"] = "mismatch"
            problems = [f"{key} {result.get(key)} != {value}" for key, value in expected.items() if key != "gap_ms" and result.get(key) != value]
            if "gap_ms" in expected and abs((result.get("audio_pts_gap_max_ms") or 0) - expected["gap_ms"]) > 1:
                problems.append(f"audio_pts_gap_max_ms {result.get('audio_pts_gap_max_ms')} != {expected['gap_ms']}")
            failures += bool(problems)
            print("ok  " if not problems else "FAIL", name, json.dumps({"legacy_ok": legacy_ok(result), **{key: result.get(key) for key in (
                "verdict", "checks", "audio_rtp_packets", "audio_timeline_packets", "audio_pts_gaps", "audio_pts_overlaps", "audio_pts_gap_max_ms",
                "audio_rtp_lost", "recorded_audio_pts_gaps", "error")}}), "; ".join(problems))
    print("PASS" if failures == 0 else "FAIL")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
