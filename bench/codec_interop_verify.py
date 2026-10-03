#!/usr/bin/env python3
"""Decode one real codec combination through the public protocol endpoints."""

import argparse
import contextlib
import json
import re
import subprocess
import time
from pathlib import Path

from fanout_support import benchmark_head, proc_snapshot, stop_process
from lifecycle_verify import Run


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--publisher", choices=("rtmp", "rtsp"), required=True)
    parser.add_argument("--rtsp-publish-transport", choices=("tcp", "udp"), default="tcp")
    parser.add_argument("--video-codec", choices=("h264", "h265"), required=True)
    parser.add_argument("--audio-codec", choices=("aac", "g711a", "g711u", "none"), required=True)
    parser.add_argument("--outputs", nargs="+", choices=("rtmp", "rtsp", "http", "hls", "gb_udp", "gb_tcp_active", "gb_tcp_passive"),
                        default=["rtmp", "rtsp", "http", "hls", "gb_udp", "gb_tcp_active", "gb_tcp_passive"])
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, help="reuse an existing codec fixture")
    parser.add_argument("--ffmpeg", type=Path, default=Path("/home/gyl/bin/ffmpeg"))
    parser.add_argument("--port-base", type=int, default=43930)
    parser.add_argument("--duration", type=int, default=3)
    args = parser.parse_args()
    if args.duration < 1:
        parser.error("duration must be positive")
    args.output.mkdir(parents=True, exist_ok=True)
    if args.fixture is None:
        args.fixture = args.output / "fixture.mkv"
        command = [str(args.ffmpeg), "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i", "testsrc2=size=320x240:rate=20"]
        if args.audio_codec != "none":
            rate = 48000 if args.audio_codec == "aac" else 8000
            command += ["-f", "lavfi", "-i", f"sine=frequency=1000:sample_rate={rate}"]
        command += ["-t", "8", "-c:v", "libx264" if args.video_codec == "h264" else "libx265", "-preset", "ultrafast"]
        if args.video_codec == "h264":
            command += ["-tune", "zerolatency", "-g", "20", "-bf", "0", "-threads", "1"]
        else:
            command += ["-x265-params", "pools=none:frame-threads=1:bframes=0:keyint=20:min-keyint=20:scenecut=0:log-level=error"]
        if args.audio_codec == "none":
            command += ["-an"]
        else:
            command += ["-c:a", {"aac": "aac", "g711a": "pcm_alaw", "g711u": "pcm_mulaw"}[args.audio_codec],
                        "-ac", "2" if args.audio_codec == "aac" else "1"]
        command.append(str(args.fixture))
        with (args.output / "fixture.log").open("w") as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=30)
        (args.output / "fixture-command.json").write_text(json.dumps(command, indent=2) + "\n")
    run = Run(args)
    result = {"head": benchmark_head(), "publisher": args.publisher, "rtsp_publish_transport": args.rtsp_publish_transport,
              "video_codec": args.video_codec, "audio_codec": args.audio_codec, "outputs": {}}
    try:
        with run.publisher(args.publisher):
            for output in args.outputs:
                transport = {"gb_udp": "udp", "gb_tcp_active": "tcp_sender_active", "gb_tcp_passive": "tcp_sender_passive"}.get(output)
                with run.pair(transport) if transport else contextlib.nullcontext("live/perf0") as name:
                    target = "rtsp" if transport else output
                    port = {"rtmp": run.rtmp_port, "rtsp": run.rtsp_port, "http": run.http_port, "hls": run.http_port}[target]
                    url = f"http://127.0.0.1:{port}/play/hls/{name}/index.m3u8" if target == "hls" else (
                        f"{target}://127.0.0.1:{port}/{name}" + (".flv" if target == "http" else ""))
                    progress = args.output / f"{output}-progress.txt"
                    command = [args.ffmpeg, "-hide_banner", "-loglevel", "info", "-xerror", "-nostats", "-progress", progress]
                    command += ["-rtsp_transport", "tcp", "-timeout", "5000000"] if target == "rtsp" else ["-rw_timeout", "5000000"]
                    command += ["-analyzeduration", "1000000", "-probesize", "500000", "-i", url, "-map", "0:v:0"]
                    if args.audio_codec != "none":
                        command += ["-map", "0:a:0", "-af", "ashowinfo"]
                    command += ["-vf", "showinfo", "-t", args.duration, "-f", "null", "-"]
                    probe = run.launch("decode-" + output, command)
                    timed_out = False
                    pressure = False
                    samples = []
                    deadline = time.monotonic() + args.duration + 20
                    while probe.poll() is None:
                        sample = {**proc_snapshot(run.server.pid), "time": time.monotonic()}
                        samples.append(sample)
                        pressure = sample["rss_kib"] > 2 * 1024 * 1024
                        if len(samples) > 1:
                            previous = samples[-2]
                            pressure = pressure or (sample["cpu"] - previous["cpu"]) / (sample["time"] - previous["time"]) > 5.4
                        timed_out = time.monotonic() > deadline
                        if timed_out or pressure:
                            stop_process(probe)
                            break
                        time.sleep(1)
                    code = probe.returncode
                    text = probe.log_path.read_text()
                    frames = [int(x) for x in re.findall(r"^frame=(\d+)$", progress.read_text(), re.M)] if progress.exists() else []
                    audio_codec = {"aac": "aac", "g711a": "pcm_alaw", "g711u": "pcm_mulaw", "none": None}[args.audio_codec]
                    video_codec = "hevc" if args.video_codec == "h265" else "h264"
                    timestamps = {}
                    for kind, marker in (("video", "showinfo"), ("audio", "ashowinfo")):
                        pts = [float(x) for x in re.findall(r"\[Parsed_" + marker + r"_[^\]]*\]\s+n:\s*\d+\s+pts:\s*-?\d+\s+pts_time:([\d.eE+-]+)", text)]
                        if pts:
                            timestamps[kind] = {"count": len(pts), "first": pts[0], "last": pts[-1], "non_monotonic": sum(b < a for a, b in zip(pts, pts[1:]))}
                    passed = (not timed_out and not pressure and code == 0 and max(frames, default=0) > 0 and "Video: " + video_codec in text
                              and timestamps.get("video", {}).get("count", 0) > 1
                              and (audio_codec is None or ("Audio: " + audio_codec in text and timestamps.get("audio", {}).get("count", 0) > 1))
                              and all(item["non_monotonic"] == 0 for item in timestamps.values()))
                    result["outputs"][output] = {"status": "PASS" if passed else "FAIL", "returncode": code, "timed_out": timed_out,
                        "resource_pressure": pressure, "resources": samples,
                        "frames": max(frames, default=0), "timestamps": timestamps, "decode_log": probe.log_path.name}
                    (args.output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
                    assert passed, (output, code, probe.log_path.name)
                    print(f"{args.publisher}/{args.rtsp_publish_transport} {args.video_codec}+{args.audio_codec} -> {output}: PASS", flush=True)
    finally:
        try:
            run.close()
        finally:
            (args.output / "commands.json").write_text(json.dumps(run.records, indent=2) + "\n")


if __name__ == "__main__":
    main()
