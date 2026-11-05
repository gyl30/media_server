#!/usr/bin/env python3
"""Check bench/whep_dump_av.py's RTP audio timeline on synthetic packets, without a dump or ffmpeg.

Covers Opus with one and two frames per packet, G.711 A-law and mu-law, header extensions, padding and CSRCs, a lost packet, a timestamp
jump and an overlap, sequence and timestamp wraparound, reordered and duplicated packets, and a dump without audio."""

import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "bench"))
import whep_dump_av

SSRC = 7
KINDS = {SSRC: "audio"}


def packet(payload_type, sequence, timestamp, payload, extension=False, padding=0, csrcs=0):
    first = 0x80 | csrcs | (0x10 if extension else 0) | (0x20 if padding else 0)
    data = struct.pack("!BBHII", first, payload_type, sequence & 0xFFFF, timestamp & 0xFFFFFFFF, SSRC) + b"\0\0\0\1" * csrcs
    if extension:
        data += struct.pack("!HH", 0xBEDE, 1) + b"\x10\xff\0\0"
    data += payload
    if padding:
        data += b"\0" * (padding - 1) + bytes([padding])
    return (sequence * 20_000_000, False, data)


def opus(sequence, timestamp, frames=1):
    return packet(111, sequence, timestamp, bytes([(31 << 3) | (0 if frames == 1 else 1), 0xAA, 0xBB]))


def pcma(sequence, timestamp, **options):
    return packet(8, sequence, timestamp, b"\xd5" * 160, **options)


def pcmu(sequence, timestamp):
    return packet(0, sequence, timestamp, b"\xff" * 160)


CASES = [
    ("opus-continuous", [opus(i, i * 960) for i in range(50)], {"audio_pts_gaps": 0, "audio_pts_overlaps": 0, "audio_rtp_lost": 0}),
    ("opus-two-frames-per-packet", [opus(i, i * 1920, 2) for i in range(50)], {"audio_pts_gaps": 0, "audio_pts_overlaps": 0, "audio_rtp_lost": 0}),
    ("opus-lost-packet", [opus(i, i * 960) for i in range(50) if i != 20], {"audio_pts_gaps": 1, "audio_rtp_lost": 1, "gap_ms": 20}),
    ("opus-timestamp-jump", [opus(i, i * 960 + (3840 if i >= 20 else 0)) for i in range(50)], {"audio_pts_gaps": 1, "audio_rtp_lost": 0, "gap_ms": 80}),
    ("opus-timestamp-overlap", [opus(i, i * 960 - (480 if i >= 20 else 0)) for i in range(50)], {"audio_pts_overlaps": 1, "audio_pts_gaps": 0, "audio_rtp_lost": 0}),
    ("opus-wrap", [opus(65_520 + i, 0xFFFFFFFF - 10 * 960 + i * 960) for i in range(50)], {"audio_pts_gaps": 0, "audio_pts_overlaps": 0, "audio_rtp_lost": 0}),
    ("opus-wrap-lost", [opus(65_520 + i, 0xFFFFFFFF - 10 * 960 + i * 960) for i in range(50) if i not in (15, 16, 17, 18)],
     {"audio_pts_gaps": 1, "audio_rtp_lost": 4, "gap_ms": 80}),
    ("opus-reordered-duplicated", [opus(i, i * 960) for i in (0, 1, 3, 2, 4, 4, 5, 6)], {"audio_pts_gaps": 0, "audio_pts_overlaps": 0, "audio_rtp_lost": 0}),
    ("pcma-extension-padding-csrc", [pcma(i, i * 160, extension=True, padding=4, csrcs=2) for i in range(50)],
     {"audio_pts_gaps": 0, "audio_pts_overlaps": 0, "audio_rtp_lost": 0}),
    ("pcma-lost-packet", [pcma(i, i * 160) for i in range(50) if i != 30], {"audio_pts_gaps": 1, "audio_rtp_lost": 1, "gap_ms": 20}),
    ("pcmu-continuous", [pcmu(i, i * 160) for i in range(50)], {"audio_pts_gaps": 0, "audio_pts_overlaps": 0, "audio_rtp_lost": 0}),
    ("no-audio", [], {"audio_timeline_packets": 0}),
]


def main():
    failures = 0
    for name, records, expected in CASES:
        result = whep_dump_av.audio_timeline(records, KINDS)
        problems = [f"{key} {result.get(key)} != {value}" for key, value in expected.items() if key != "gap_ms" and result.get(key) != value]
        if "gap_ms" in expected and abs(result.get("audio_pts_gap_max_ms", 0) - expected["gap_ms"]) > 0.01:
            problems.append(f"audio_pts_gap_max_ms {result.get('audio_pts_gap_max_ms')} != {expected['gap_ms']}")
        failures += bool(problems)
        print("ok  " if not problems else "FAIL", name, "; ".join(problems))
    print("PASS" if failures == 0 else "FAIL")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
