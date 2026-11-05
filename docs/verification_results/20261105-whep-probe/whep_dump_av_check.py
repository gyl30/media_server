#!/usr/bin/env python3
"""Replay variants of one recorded whep_fanout dump through bench/whep_dump_av.py and check its audio timeline verdicts.

The variants keep every media packet of the source dump and only change what each case names: the first audio RTCP Sender Report moved
50 ms after or 1 ms before the first audio RTP packet, four consecutive audio packets removed, a +80 ms RTP timestamp step and a -20 ms
RTP timestamp step. Removed packets and timestamp steps are placed ten seconds into the replay window."""

import argparse
import json
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

AUDIO_PAYLOAD_TYPE = 111


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
    return not record[1] and len(record[2]) >= 12 and (record[2][1] & 0x7F) == AUDIO_PAYLOAD_TYPE


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


CASES = [
    ("original", lambda records: records, {"audio_pts_gaps": 0, "audio_pts_overlaps": 0, "audio_rtp_lost": 0}),
    ("sr-after-first-audio-50ms", lambda records: move_report(records, 50_000_000), {"audio_pts_gaps": 0, "audio_pts_overlaps": 0, "audio_rtp_lost": 0}),
    ("sr-before-first-audio-1ms", lambda records: move_report(records, -1_000_000), {"audio_pts_gaps": 0, "audio_pts_overlaps": 0, "audio_rtp_lost": 0}),
    ("drop-4-audio", lambda records: drop_audio(records, 4), {"audio_pts_gaps": 1, "audio_pts_overlaps": 0, "audio_rtp_lost": 4, "gap_ms": 80}),
    ("timestamp-step-plus-80ms", lambda records: step_timestamps(records, 3840), {"audio_pts_gaps": 1, "audio_pts_overlaps": 0, "audio_rtp_lost": 0, "gap_ms": 80}),
    ("timestamp-step-minus-20ms", lambda records: step_timestamps(records, -960), {"audio_pts_gaps": 0, "audio_pts_overlaps": 1, "audio_rtp_lost": 0}),
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dump")
    parser.add_argument("--tool", type=Path, default=Path(__file__).resolve().parents[3] / "bench" / "whep_dump_av.py")
    parser.add_argument("--ffmpeg-bin", default="ffmpeg")
    parser.add_argument("--seconds", type=float, default=25)
    parser.add_argument("--only", nargs="*")
    args = parser.parse_args()

    source = read_dump(args.dump)
    failures = 0
    with tempfile.TemporaryDirectory() as directory:
        for name, build, expected in CASES:
            if args.only and name not in args.only:
                continue
            dump = Path(directory) / f"{name}.dump"
            write_dump(dump, build(source))
            completed = subprocess.run([sys.executable, str(args.tool), str(dump), "--seconds", str(args.seconds), "--output", str(dump.with_suffix(".mkv")),
                                        "--ffmpeg-bin", args.ffmpeg_bin], capture_output=True, text=True)
            result = json.loads(completed.stdout) if completed.returncode == 0 and completed.stdout.strip() else {"error": completed.stderr[-300:]}
            problems = [f"{key} {result.get(key)} != {value}" for key, value in expected.items() if key != "gap_ms" and result.get(key) != value]
            if "gap_ms" in expected and abs((result.get("audio_pts_gap_max_ms") or 0) - expected["gap_ms"]) > 1:
                problems.append(f"audio_pts_gap_max_ms {result.get('audio_pts_gap_max_ms')} != {expected['gap_ms']}")
            failures += bool(problems)
            print("ok  " if not problems else "FAIL", name, json.dumps({key: result.get(key) for key in (
                "audio_frames", "audio_pts_gaps", "audio_pts_overlaps", "audio_pts_gap_max_ms", "audio_rtp_lost",
                "recorded_audio_pts_gaps", "recorded_audio_pts_gap_max_ms", "error")}), "; ".join(problems))
    print("PASS" if failures == 0 else "FAIL")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
