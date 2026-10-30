#!/usr/bin/env python3
"""Replay a whep_fanout --dump-file to ffmpeg over loopback RTP at the original pace, record it and report av_probe metrics.

The output timeline is judged from the RTP timestamps in the dump; the recording's own timestamp order is kept as recorded_video_pts_backward,
because ffmpeg clamps a first frame that maps before the other stream's start. B-frames arrive in decode order, so a video timestamp only counts as
backward when it falls more than MAX_REORDER_TICKS below the latest one, and the recorder re-encodes such video because RTP carries no DTS.
G.711 audio is recorded as PCM because Matroska has no copy mapping for it."""

import argparse
import json
import signal
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path

VIDEO_PAYLOAD_TYPES = (102, 104, 106, 108)
AUDIO_RTPMAPS = {111: "opus/48000/2", 8: "PCMA/8000", 0: "PCMU/8000"}
MAX_REORDER_TICKS = 45000


def read_dump(path):
    data = Path(path).read_bytes()
    records = []
    offset = 0
    while offset + 11 <= len(data):
        nanoseconds, rtcp, size = struct.unpack_from("<qBH", data, offset)
        offset += 11
        records.append((nanoseconds, bool(rtcp), data[offset:offset + size]))
        offset += size
    return records


def free_port_block():
    for base in range(20000, 32768, 4):
        sockets = []
        try:
            for port in range(base, base + 4):
                sockets.append(socket.socket(socket.AF_INET, socket.SOCK_DGRAM))
                sockets[-1].bind(("127.0.0.1", port))
            return base
        except OSError:
            continue
        finally:
            for item in sockets:
                item.close()
    sys.exit("no free loopback port block")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dump")
    parser.add_argument("--output", type=Path, required=True, help="recorded .mkv")
    parser.add_argument("--ffmpeg-bin", default="ffmpeg")
    parser.add_argument("--base-port", type=int, default=0, help="first of four loopback ports; 0 picks a free block below the ephemeral range")
    parser.add_argument("--seconds", type=float, default=0, help="replay only the first SECONDS of the dump; 0 replays all")
    args = parser.parse_args()

    records = read_dump(args.dump)
    if args.seconds > 0 and records:
        records = [record for record in records if record[0] - records[0][0] <= args.seconds * 1e9]
    ssrc_kind = {}
    video_payload_type = audio_payload_type = None
    for _, rtcp, packet in records:
        if rtcp or len(packet) < 12:
            continue
        payload_type = packet[1] & 0x7F
        kind = "video" if payload_type in VIDEO_PAYLOAD_TYPES else "audio" if payload_type in AUDIO_RTPMAPS else None
        if kind:
            ssrc_kind.setdefault(struct.unpack_from("!I", packet, 8)[0], kind)
            video_payload_type = payload_type if kind == "video" else video_payload_type
            audio_payload_type = payload_type if kind == "audio" else audio_payload_type
    if video_payload_type is None:
        sys.exit("dump has no video RTP")
    base_port = args.base_port or free_port_block()
    ports = {"video": base_port, "audio": base_port + 2}
    sdp = args.output.with_suffix(".sdp")
    lines = ["v=0", "o=- 0 0 IN IP4 127.0.0.1", "s=whep", "c=IN IP4 127.0.0.1", "t=0 0",
             f"m=video {ports['video']} RTP/AVP {video_payload_type}", f"a=rtpmap:{video_payload_type} "
             f"{'H265' if video_payload_type == 104 else 'H264'}/90000"]
    if video_payload_type != 104:
        lines.append(f"a=fmtp:{video_payload_type} packetization-mode=1")
    if audio_payload_type is not None:
        lines += [f"m=audio {ports['audio']} RTP/AVP {audio_payload_type}", f"a=rtpmap:{audio_payload_type} {AUDIO_RTPMAPS[audio_payload_type]}"]
    sdp.write_text("\r\n".join(lines) + "\r\n")

    previous = {}
    rtp_backward = rtp_reordered = 0
    for _, rtcp, packet in records:
        if rtcp or len(packet) < 12 or ssrc_kind.get(struct.unpack_from("!I", packet, 8)[0]) != "video":
            continue
        timestamp = struct.unpack_from("!I", packet, 4)[0]
        if "video" in previous:
            rtp_reordered += (timestamp - previous["video"]) & 0xFFFFFFFF >= 0x80000000
            rtp_backward += (timestamp - previous["latest"]) & 0xFFFFFFFF >= 0x80000000 and (previous["latest"] - timestamp) & 0xFFFFFFFF > MAX_REORDER_TICKS
            if (timestamp - previous["latest"]) & 0xFFFFFFFF < 0x80000000:
                previous["latest"] = timestamp
        else:
            previous["latest"] = timestamp
        previous["video"] = timestamp
    video_codec = ["-c:v", "libx264", "-preset", "ultrafast", "-crf", "18"] if rtp_reordered else ["-c:v", "copy"]
    audio_codec = ["-c:a", "copy"] if audio_payload_type == 111 else ["-c:a", "pcm_s16le"]
    recorder = subprocess.Popen([args.ffmpeg_bin, "-hide_banner", "-loglevel", "error", "-y", "-protocol_whitelist", "file,udp,rtp",
                                 "-i", str(sdp), "-map", "0", *video_codec, *audio_codec, str(args.output)], stdin=subprocess.DEVNULL)
    time.sleep(1.5)
    sender = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    started = time.monotonic_ns() - records[0][0]
    sent = 0
    for nanoseconds, rtcp, packet in records:
        if len(packet) < 8:
            continue
        ssrc = struct.unpack_from("!I", packet, 4 if rtcp else 8)[0]
        kind = ssrc_kind.get(ssrc)
        if kind is None:
            continue
        delay = (started + nanoseconds - time.monotonic_ns()) / 1e9
        if delay > 0:
            time.sleep(delay)
        sender.sendto(packet, ("127.0.0.1", ports[kind] + (1 if rtcp else 0)))
        sent += 1
    time.sleep(1)
    recorder.send_signal(signal.SIGINT)
    recorder.wait(timeout=30)
    probe = subprocess.run([sys.executable, str(Path(__file__).with_name("av_probe.py")), str(args.output), "--seconds", "3600",
                            "--ffmpeg-bin", args.ffmpeg_bin], capture_output=True, text=True)
    result = json.loads(probe.stdout) if probe.returncode == 0 and probe.stdout.strip() else {"error": probe.stderr[-500:]}
    result["video_rtp_reordered"] = rtp_reordered
    result["recorded_video_pts_backward"] = result.get("video_pts_backward")
    result["video_pts_backward"] = rtp_backward
    result["replayed_packets"] = sent
    result["recorder_exit"] = recorder.returncode
    print(json.dumps(result))


if __name__ == "__main__":
    main()
