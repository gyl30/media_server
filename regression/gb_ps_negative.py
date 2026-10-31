#!/usr/bin/env python3
"""Mutate a good GB28181 RTP/PS dump (gb_fanout --dump-file) and require bench/gb_av_probe.py to reject every mutation while still
accepting the unmodified dump. Mutations: audio packs removed, the audio stream type in the PSM replaced, an audio PES truncated,
an invalid RTP version, a missing RTP packet, corrupted audio samples, the tail of a video pack removed with RTP sequence numbers kept
continuous, and a dump cut inside its last RTP record. A capture that ends inside its last PS pack is a normal capture end and must still
be accepted."""

import argparse
import json
import struct
import subprocess
import sys
from pathlib import Path


def read_records(data):
    records = []
    offset = 0
    while offset + 2 <= len(data):
        length = struct.unpack_from("!H", data, offset)[0]
        records.append(bytearray(data[offset + 2:offset + 2 + length]))
        offset += 2 + length
    return records


def encode_records(records):
    return b"".join(struct.pack("!H", len(record)) + bytes(record) for record in records)


def payload_offset(record):
    offset = 12 + 4 * (record[0] & 0x0F)
    if record[0] & 0x10:
        offset += 4 + 4 * struct.unpack_from("!H", record, offset + 2)[0]
    return offset


def pack_elements(record):
    data = record
    offset = payload_offset(record)
    if data[offset:offset + 4] != b"\x00\x00\x01\xba":
        return
    offset += 14 + (data[offset + 13] & 7)
    while offset + 6 <= len(data) and data[offset:offset + 3] == b"\x00\x00\x01":
        length = struct.unpack_from("!H", data, offset + 4)[0]
        yield offset, data[offset + 3], length
        offset += 6 + length


def packs(records):
    starts = [index for index, record in enumerate(records) if record[payload_offset(record):payload_offset(record) + 4] == b"\x00\x00\x01\xba"]
    return [range(start, end) for start, end in zip(starts, starts[1:] + [len(records)])]


def audio_packets(records):
    indices = []
    for index, record in enumerate(records):
        elements = list(pack_elements(record))
        if elements and 0xC0 <= elements[-1][1] <= 0xDF and elements[-1][0] + 6 + elements[-1][2] == len(record):
            indices.append(index)
    return indices


def renumber(records):
    first = struct.unpack_from("!H", records[0], 2)[0]
    for index, record in enumerate(records):
        struct.pack_into("!H", record, 2, (first + index) & 0xFFFF)


def mutations(records):
    audio = audio_packets(records)
    middle = audio[len(audio) // 2:]
    yield "unmodified", [bytearray(record) for record in records]

    kept = [bytearray(record) for index, record in enumerate(records) if index not in set(audio)]
    renumber(kept)
    yield "audio_packs_removed", kept

    changed = [bytearray(record) for record in records]
    replaced = 0
    for record in changed:
        for start, stream_id, length in pack_elements(record):
            if stream_id != 0xBC:
                continue
            info = struct.unpack_from("!H", record, start + 8)[0]
            entry = start + 10 + info + 2
            end = start + 6 + length - 4
            while entry + 4 <= end:
                if 0xC0 <= record[entry + 1] <= 0xDF:
                    record[entry] = 0x00
                    replaced += 1
                entry += 4 + struct.unpack_from("!H", record, entry + 2)[0]
    if replaced:
        yield "psm_audio_type_invalid", changed

    truncated = [bytearray(record) for record in records]
    for index in middle[:3]:
        del truncated[index][-8:]
    yield "audio_pes_truncated", truncated

    version = [bytearray(record) for record in records]
    version[len(version) // 2][0] = (version[len(version) // 2][0] & 0x3F) | 0x40
    yield "rtp_version_invalid", version

    gap = [bytearray(record) for index, record in enumerate(records) if index != len(records) // 2]
    yield "rtp_packet_missing", gap

    corrupted = [bytearray(record) for record in records]
    for index in middle[:25]:
        start, _, length = list(pack_elements(corrupted[index]))[-1]
        header = 9 + corrupted[index][start + 8]
        body = range(start + header + 9, start + 6 + length)
        for position in list(body)[::3]:
            corrupted[index][position] ^= 0x5A
    yield "audio_samples_corrupted", corrupted

    groups = packs(records)
    video = [group for group in groups[len(groups) // 2:-1] if len(group) >= 3 and any(0xE0 <= stream_id <= 0xEF for _, stream_id, _ in
                                                                                        pack_elements(records[group[0]]))]
    removed = [bytearray(record) for index, record in enumerate(records) if index != video[0][-1]]
    renumber(removed)
    yield "video_pack_tail_removed", removed

    last = next(group for group in reversed(groups) if len(group) >= 2)
    yield "capture_ends_mid_pack", [bytearray(record) for record in records[:last[0] + 1]]

    yield "capture_ends_mid_record", encode_records(records)[:-5]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dump", type=Path)
    parser.add_argument("--output", type=Path, required=True, help="new directory for the mutated dumps and probe results")
    parser.add_argument("--demux-bin", type=Path, required=True)
    parser.add_argument("--ffmpeg-bin", default="ffmpeg")
    parser.add_argument("--reference", type=Path, required=True, help="source fixture of the dump")
    args = parser.parse_args()
    args.output.mkdir(parents=True)
    records = read_records(args.dump.read_bytes())
    failures = []
    for name, mutated in mutations(records):
        path = args.output / f"{name}.rtp"
        path.write_bytes(mutated if isinstance(mutated, bytes) else encode_records(mutated))
        process = subprocess.run([sys.executable, str(Path(__file__).resolve().parent.parent / "bench/gb_av_probe.py"), str(path), "--demux-bin",
                                  str(args.demux_bin), "--ffmpeg-bin", args.ffmpeg_bin, "--expect-audio", "--reference", str(args.reference)],
                                 capture_output=True, text=True)
        (args.output / f"{name}.json").write_text(process.stdout)
        try:
            result = json.loads(process.stdout)
        except ValueError:
            result = {"ok": False, "checks": {"probe_output": False}}
        accepted = result.get("ok") is True
        expected = name in ("unmodified", "capture_ends_mid_pack")
        failed_checks = sorted(key for key, value in (result.get("checks") or {}).items() if not value)
        print(json.dumps({"mutation": name, "accepted": accepted, "expected_accept": expected, "failed_checks": failed_checks,
                          "demux_errors": (result.get("demux") or {}).get("errors", [])[:2]}))
        if accepted != expected:
            failures.append(name)
    print("GB_PS_NEGATIVE", "PASS" if not failures else f"FAIL {failures}")
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
