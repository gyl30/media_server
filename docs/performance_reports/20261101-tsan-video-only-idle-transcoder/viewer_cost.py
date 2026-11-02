#!/usr/bin/env python3
"""Run WHIP sources on one server and measure a sequence of viewer steps: CPU cycles, PSS, Opus->AAC transcoder starts and
destructions, and the viewers' own readiness. Steps: idle, video (video-only WHEP offer), passthrough (WHEP offering Opus
stereo=1), video_flv (video-only WHEP plus one HTTP-FLV each), flv:K (HTTP-FLV on the first K sources), and video_stop as the last step:
video-only WHEP viewers that stay connected while the publisher stops, to check that their sessions end with the source."""

import argparse
import json
import re
import subprocess
import sys
import time
from pathlib import Path


def pss_kib(pid):
    return sum(int(line.split()[1]) for line in Path(f"/proc/{pid}/smaps_rollup").read_text().splitlines() if line.startswith("Pss:"))


def trace_counts(path, until):
    counts = (0, 0)
    for line in path.read_text().splitlines():
        fields = line.split()
        if len(fields) == 3 and fields[0].isdigit():
            if int(fields[0]) > until * 1e9:
                break
            counts = (int(fields[1]), int(fields[2]))
    return counts


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--server-bin", required=True)
    parser.add_argument("--client-dir", default="build-t")
    parser.add_argument("--sources", type=int, default=64)
    parser.add_argument("--steps", default="idle,video,passthrough,video_flv,idle")
    parser.add_argument("--workers", type=int, default=8)
    parser.add_argument("--settle", type=int, default=20)
    parser.add_argument("--window", type=int, default=30)
    parser.add_argument("--trace", type=Path)
    parser.add_argument("--host", default="198.19.0.10")
    parser.add_argument("--client-address", default="198.19.0.11")
    parser.add_argument("--server-cpus", default="0-7")
    parser.add_argument("--client-cpus", default="8-15,24-31")
    args = parser.parse_args()
    runtime = args.runtime.resolve()
    sys.path.insert(0, str(runtime / "bench"))
    from signaling_stub import SignalingStub
    from perf_run import request
    args.output.mkdir(parents=True)
    env = {"LD_LIBRARY_PATH": str(runtime / "lib")}
    token = "viewer-cost"
    clients = runtime / args.client_dir
    server_bin = str(runtime / args.server_bin)
    processes = []
    result = {"server_bin": args.server_bin, "sources": args.sources, "steps": []}
    with SignalingStub() as stub:
        server = subprocess.Popen(["taskset", "-c", args.server_cpus, server_bin, "--threads", str(args.workers), "--bind-address", args.host,
                                   "--webrtc-address", args.host, "--rtmp-port", "11935", "--rtsp-port", "18554", "--http-port", "18080",
                                   "--control-token", token, "--signaling-url", stub.url], stdout=(args.output / "server.log").open("w"),
                                  stderr=subprocess.STDOUT, env=env)
        processes.append(server)
        trace_log = args.output / "trace.log"
        if args.trace:
            processes.append(subprocess.Popen(["bpftrace", str(args.trace), server_bin], stdout=trace_log.open("w"), stderr=subprocess.STDOUT))
        time.sleep(2)
        streams = [stub.issue("publish", ttl=600) for _ in range(args.sources)]
        (args.output / "publish.urls").write_text("".join(f"http://{args.host}:18080/publish/whip/{stream}\n" for stream in streams))
        publisher = subprocess.Popen(["taskset", "-c", args.client_cpus, str(clients / "whip_publisher"), "--whip-urls-file", str(args.output / "publish.urls"),
                                      "--fixture", str(runtime / ".cache/perf_fixture_whip.mkv"), "--sources", str(args.sources), "--bind-address",
                                      args.client_address, "--io-threads", "4", "--ramp-per-second", "200", "--duration", "86400"],
                                     stdout=(args.output / "publisher.log").open("w"), stderr=subprocess.STDOUT, env=env)
        processes.append(publisher)
        deadline = time.monotonic() + 90
        ready = set()
        while time.monotonic() < deadline and len(ready) < args.sources:
            ready = {item["stream_id"] for item in request(f"http://{args.host}:18080", "/receivers", token=token)["receivers"]} & set(streams)
            time.sleep(0.5)
        result["ready"] = len(ready)

        def viewers(kind, selected, index, duration):
            name = f"{index}-{kind}"
            common = ["--viewers", str(len(selected)), "--ramp-per-second", "200", "--ready-timeout", "90", "--warmup", "0",
                      "--duration", str(duration), "--progress-interval", "5", "--start-file", str(args.output / f"{index}.start")]
            path = args.output / f"{name}.credentials"
            if kind == "flv":
                path.write_text("".join(f"{stream}/{stub.issue('play', stream, ttl=600)}.flv\n" for stream in selected))
                command = [str(clients / "http_flv_fanout"), "--play-paths-file", str(path), "--media-host", args.host, "--media-port", "18080", *common]
            else:
                path.write_text("".join(f"http://{args.host}:18080/play/whep/{stream}/{stub.issue('play', stream, ttl=600)}\n" for stream in selected))
                command = [str(clients / "whep_fanout"), "--whep-urls-file", str(path), "--bind-address", args.client_address, "--io-threads", "2",
                           *(["--video-only"] if kind in ("video", "video_stop") else []), *common]
            log = args.output / f"{name}.log"
            return log, subprocess.Popen(["taskset", "-c", args.client_cpus, *command], stdout=log.open("w"), stderr=subprocess.STDOUT, env=env)

        for index, step in enumerate(args.steps.split(",")):
            kind, _, count = step.partition(":")
            selected = streams[:int(count)] if count else streams
            groups = {"idle": [], "video": ["video"], "passthrough": ["passthrough"], "video_flv": ["video", "flv"], "flv": ["flv"],
                      "video_stop": ["video_stop"]}[kind]
            started = time.monotonic()
            launched = [viewers(group, selected, index, args.window + (120 if kind == "video_stop" else 5)) for group in groups]
            time.sleep(len(selected) / 200 + 10 if launched else 0)
            start_ns = time.monotonic_ns() + args.settle * 1_000_000_000
            (args.output / f"{index}.start").write_text(f"{start_ns}\n")
            time.sleep(max(0, (start_ns - time.monotonic_ns()) / 1e9))
            window_started = time.monotonic()
            stat = subprocess.run(["perf", "stat", "-x,", "-e", "cycles,instructions", "-p", str(server.pid), "--", "sleep", str(args.window)],
                                  capture_output=True, text=True)
            counters = {line.split(",")[2]: int(line.split(",")[0]) for line in stat.stderr.splitlines() if line and line.split(",")[0].isdigit()}
            entry = {"step": step, "gcycles_per_second": round(counters.get("cycles", 0) / 1e9 / args.window, 3),
                     "ginstructions_per_second": round(counters.get("instructions", 0) / 1e9 / args.window, 3), "pss_mib": round(pss_kib(server.pid) / 1024, 1)}
            if kind == "video_stop":
                shutdown_before = (args.output / "server.log").read_text(errors="replace").count("webrtc whep session shutdown")
                publisher.terminate()
                stopped = time.monotonic()
                launched[0][1].wait(timeout=180)
                time.sleep(5)
                log_text = (args.output / "server.log").read_text(errors="replace")
                entry["stop"] = {"viewers_exit_after_s": round(time.monotonic() - stopped - 5, 1),
                                 "whep_shutdown": log_text.count("webrtc whep session shutdown") - shutdown_before,
                                 "source_ended": log_text.count("webrtc source stream ended"),
                                 "receivers_left": len({item["stream_id"] for item in request(f"http://{args.host}:18080", "/receivers", token=token)["receivers"]} & set(streams))}
            for log, process in launched:
                process.wait(timeout=180)
                text = log.read_text()
                measurement = [line for line in text.splitlines() if line.startswith("phase=measurement ")]
                fields = dict(re.findall(r"(\w+)=(\S+)", measurement[-1])) if measurement else {}
                entry[log.stem.split("-", 1)[1]] = {"exit": process.returncode, **{key: fields[key] for key in
                    ("ready", "progressing", "failed", "runtime_failures", "audio_packets", "video_packets", "steady_audio_messages",
                     "steady_video_messages", "viewer_audio_packets_min", "viewer_video_packets_min") if key in fields}}
            ended = time.monotonic()
            if args.trace:
                time.sleep(1)
                before, during_start, after = trace_counts(trace_log, started), trace_counts(trace_log, window_started), trace_counts(trace_log, ended)
                entry["opus_aac_starts_step"] = after[0] - before[0]
                entry["opus_aac_starts_window"] = after[0] - during_start[0]
                entry["transcoder_destroys_step"] = after[1] - before[1]
                entry["opus_aac_starts_total"] = after[0]
            result["steps"].append(entry)
            print(json.dumps(entry), flush=True)
        result["publisher_alive"] = publisher.poll() is None
        result["still_ready"] = len({item["stream_id"] for item in request(f"http://{args.host}:18080", "/receivers", token=token)["receivers"]} & set(streams))
        for process in reversed(processes):
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=20)
                except subprocess.TimeoutExpired:
                    process.kill()
        result["server_exit"] = server.returncode
    (args.output / "result.json").write_text(json.dumps(result, indent=2))
    print(json.dumps({key: value for key, value in result.items() if key != "steps"}))


main()
