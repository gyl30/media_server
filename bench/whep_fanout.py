#!/usr/bin/env python3

import argparse
import json
import platform
import statistics
import subprocess
import time
from pathlib import Path

from fanout_support import benchmark_head, parse_phase, proc_snapshot, stop_process, thread_rates, wait_for_listener, wait_for_phase, wait_for_stream


def main():
    parser = argparse.ArgumentParser(description="Direct RTMP publish to WHEP fanout benchmark")
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--server-bin", type=Path, default=Path("build/media_server"))
    parser.add_argument("--client-bin", type=Path, default=Path("build/whep_fanout"))
    parser.add_argument("--ffmpeg-bin", type=Path, default=Path("/home/gyl/bin/ffmpeg"))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--viewers", type=int, required=True)
    parser.add_argument("--sources", type=int, default=1)
    parser.add_argument("--workers", type=int, default=6)
    parser.add_argument("--video-codec", choices=("passthrough", "av1"), default="passthrough")
    parser.add_argument("--client-threads", type=int, default=8)
    parser.add_argument("--ramp-per-second", type=int, default=100)
    parser.add_argument("--warmup", type=int, default=5)
    parser.add_argument("--duration", type=int, default=20)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--rtmp-port", type=int, default=11935)
    parser.add_argument("--rtsp-port", type=int, default=18554)
    parser.add_argument("--http-port", type=int, default=18080)
    args = parser.parse_args()
    if min(args.viewers, args.sources, args.workers, args.client_threads, args.ramp_per_second, args.duration) < 1 or args.warmup < 0:
        parser.error("viewers, workers, client-threads, ramp-per-second and duration must be positive")

    args.output.mkdir(parents=True, exist_ok=True)
    server_log_path = args.output / "server.log"
    client_log_path = args.output / "client.log"
    with server_log_path.open("w") as server_log, (args.output / "publisher.log").open("w") as publisher_log, \
         client_log_path.open("w") as client_log, (args.output / "client-error.log").open("w") as client_error:
        server = subprocess.Popen(
            [str(args.server_bin), "--threads", str(args.workers), "--rtmp-port", str(args.rtmp_port),
             "--rtsp-port", str(args.rtsp_port), "--http-port", str(args.http_port), "--bind-address", args.host,
             "--webrtc-address", args.host, "--whep-video-codec", args.video_codec], stdout=server_log, stderr=subprocess.STDOUT,
        )
        publisher = None
        client = None
        try:
            wait_for_listener(args.host, args.rtmp_port)
            addresses = [f"rtmp://{args.host}:{args.rtmp_port}/live/perf{index}" for index in range(args.sources)]
            output = addresses[0] if args.sources == 1 else "|".join(f"[f=flv]{address}" for address in addresses)
            publisher = subprocess.Popen(
                [str(args.ffmpeg_bin), "-hide_banner", "-loglevel", "error", "-stream_loop", "-1", "-re", "-i", str(args.fixture),
                 "-map", "0:v:0", "-map", "0:a:0", "-c", "copy", "-f", "flv" if args.sources == 1 else "tee", output],
                stdout=publisher_log, stderr=subprocess.STDOUT,
            )
            for index in range(args.sources):
                wait_for_stream(args.host, args.http_port, f"live/perf{index}")
            client = subprocess.Popen(
                [str(args.client_bin), "--whep-url", f"http://{args.host}:{args.http_port}/play/whep/live/perf0",
                 "--viewers", str(args.viewers), "--sources", str(args.sources), "--io-threads", str(args.client_threads),
                 "--ramp-per-second", str(args.ramp_per_second), "--warmup", str(args.warmup),
                 "--duration", str(args.duration), "--video-codec", args.video_codec], stdout=client_log, stderr=client_error,
            )
            established = parse_phase(wait_for_phase(client_log_path, client, "established", max(90, args.viewers / 10)))
            wait_for_phase(client_log_path, client, "measurement_start", args.warmup + 30)
            before_server = proc_snapshot(server.pid)
            samples = []
            started = time.monotonic()
            while time.monotonic() - started < args.duration:
                time.sleep(min(1, max(0, args.duration - (time.monotonic() - started))))
                samples.append(proc_snapshot(server.pid))
            elapsed = time.monotonic() - started
            after_server = proc_snapshot(server.pid)
            measurement = parse_phase(wait_for_phase(client_log_path, client, "measurement", 30))
            client.wait(timeout=max(30, args.viewers / 10))
            disconnect = parse_phase(wait_for_phase(client_log_path, client, "disconnect", 5))
            result = {
                "config": {
                    "head": benchmark_head(),
                    "fixture": str(args.fixture), "viewers": args.viewers, "sources": args.sources, "workers": args.workers,
                    "video_codec": args.video_codec,
                    "client_threads": args.client_threads, "ramp_per_second": args.ramp_per_second,
                    "warmup_seconds": args.warmup, "duration_seconds": args.duration,
                    "kernel": platform.release(),
                },
                "established": established,
                "measurement": measurement,
                "disconnect": disconnect,
                "client_exit": client.returncode,
                "server_cpu_cores": (after_server["cpu"] - before_server["cpu"]) / elapsed,
                "server_rss_kib_median": statistics.median(sample["rss_kib"] for sample in samples),
                "server_pss_kib_median": statistics.median(sample["pss_kib"] for sample in samples),
                "server_fd_median": statistics.median(sample["fd"] for sample in samples),
                "server_thread_cpu_cores": thread_rates(before_server, after_server, "cpu", elapsed),
                "server_thread_context_switches_per_second": thread_rates(before_server, after_server, "context_switches", elapsed),
                "server_thread_migrations_per_second": thread_rates(before_server, after_server, "migrations", elapsed),
                "client_cpu_cores": measurement["generator_cpu_cores"],
                "queue_full_events": server_log_path.read_text().count("whep udp write queue full"),
            }
            (args.output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
            print(json.dumps(result, indent=2))
            if client.returncode != 0 or established["media_ready"] != args.viewers or measurement["progressing"] != args.viewers:
                raise SystemExit(1)
        finally:
            stop_process(client)
            stop_process(publisher)
            stop_process(server)


if __name__ == "__main__":
    main()
