#!/usr/bin/env python3

import argparse
import json
import platform
import statistics
import subprocess
import sys
import time
from pathlib import Path

from fanout_support import benchmark_head, parse_phase, proc_snapshot, stop_process, thread_rates, wait_for_listener, wait_for_phase, wait_for_stream


def main():
    parser = argparse.ArgumentParser(description="RTMP, RTSP, HLS and WHEP fanout from one source")
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--server-bin", type=Path, default=Path("build/media_server"))
    parser.add_argument("--ffmpeg-bin", type=Path, default=Path("/home/gyl/bin/ffmpeg"))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--viewers-per-protocol", type=int, required=True)
    parser.add_argument("--workers", type=int, default=6)
    parser.add_argument("--warmup", type=int, default=5)
    parser.add_argument("--duration", type=int, default=20)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--rtmp-port", type=int, default=11935)
    parser.add_argument("--rtsp-port", type=int, default=18554)
    parser.add_argument("--http-port", type=int, default=18080)
    args = parser.parse_args()
    if min(args.viewers_per_protocol, args.workers, args.duration) < 1 or args.warmup < 0:
        parser.error("viewers, workers and duration must be positive; warmup must be nonnegative")

    args.output.mkdir(parents=True, exist_ok=True)
    viewers = str(args.viewers_per_protocol)
    client_duration = str(args.duration + 10)
    common = ["--viewers", viewers, "--ramp-per-second", "100", "--warmup", str(args.warmup), "--duration", client_duration]
    commands = {
        "rtmp": ["build/rtmp_fanout", "--stream-name", "live/perf0", "--media-host", args.host,
                 "--media-port", str(args.rtmp_port), *common],
        "rtsp": ["build/rtsp_fanout", "--stream-name", "live/perf0", "--media-host", args.host,
                 "--media-port", str(args.rtsp_port), *common],
        "whep": ["build/whep_fanout", "--whep-url", f"http://{args.host}:{args.http_port}/play/whep/live/perf0",
                 "--io-threads", "8", *common],
        "hls": [sys.executable, str(Path(__file__).with_name("hls_client.py")),
                "--hls-url", f"http://{args.host}:{args.http_port}/play/hls/live/perf0/index.m3u8", *common],
    }
    logs = {}
    clients = {}
    with (args.output / "server.log").open("w") as server_log, (args.output / "publisher.log").open("w") as publisher_log:
        server = subprocess.Popen(
            [str(args.server_bin), "--threads", str(args.workers), "--rtmp-port", str(args.rtmp_port),
             "--rtsp-port", str(args.rtsp_port), "--http-port", str(args.http_port), "--bind-address", args.host,
             "--webrtc-address", args.host], stdout=server_log, stderr=subprocess.STDOUT,
        )
        publisher = None
        try:
            wait_for_listener(args.host, args.rtmp_port)
            publisher = subprocess.Popen(
                [str(args.ffmpeg_bin), "-hide_banner", "-loglevel", "error", "-stream_loop", "-1", "-re", "-i", str(args.fixture),
                 "-c", "copy", "-f", "flv", f"rtmp://{args.host}:{args.rtmp_port}/live/perf0"],
                stdout=publisher_log, stderr=subprocess.STDOUT,
            )
            wait_for_stream(args.host, args.http_port)
            for protocol, command in commands.items():
                log = (args.output / f"{protocol}.log").open("w")
                error_log = (args.output / f"{protocol}-error.log").open("w")
                logs[protocol] = (log, error_log)
                clients[protocol] = subprocess.Popen(command, stdout=log, stderr=error_log)
            for protocol, client in clients.items():
                wait_for_phase(args.output / f"{protocol}.log", client, "measurement_start", 90)
            before = proc_snapshot(server.pid)
            samples = []
            started = time.monotonic()
            while time.monotonic() - started < args.duration:
                time.sleep(min(1, max(0, args.duration - (time.monotonic() - started))))
                samples.append(proc_snapshot(server.pid))
            elapsed = time.monotonic() - started
            after = proc_snapshot(server.pid)
            measurements = {}
            for protocol, client in clients.items():
                path = args.output / f"{protocol}.log"
                measurements[protocol] = parse_phase(wait_for_phase(path, client, "measurement", 30))
                client.wait(timeout=30)
            result = {
                "config": {
                    "head": benchmark_head(),
                    "fixture": str(args.fixture), "viewers_per_protocol": args.viewers_per_protocol,
                    "workers": args.workers, "warmup_seconds": args.warmup,
                    "server_measurement_seconds": args.duration,
                    "client_measurement_seconds": args.duration + 10,
                    "kernel": platform.release(),
                },
                "measurements": measurements,
                "client_exits": {protocol: client.returncode for protocol, client in clients.items()},
                "server_cpu_cores": (after["cpu"] - before["cpu"]) / elapsed,
                "server_rss_kib_median": statistics.median(sample["rss_kib"] for sample in samples),
                "server_pss_kib_median": statistics.median(sample["pss_kib"] for sample in samples),
                "server_fd_median": statistics.median(sample["fd"] for sample in samples),
                "server_thread_cpu_cores": thread_rates(before, after, "cpu", elapsed),
                "server_thread_context_switches_per_second": thread_rates(before, after, "context_switches", elapsed),
                "server_thread_migrations_per_second": thread_rates(before, after, "migrations", elapsed),
                "queue_full_events": (args.output / "server.log").read_text().count("whep udp write queue full"),
            }
            (args.output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
            print(json.dumps(result, indent=2))
            if any(code != 0 for code in result["client_exits"].values()):
                raise SystemExit(1)
        finally:
            for client in clients.values():
                stop_process(client)
            stop_process(publisher)
            stop_process(server)
            for log, error_log in logs.values():
                log.close()
                error_log.close()


if __name__ == "__main__":
    main()
