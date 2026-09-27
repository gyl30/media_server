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
    parser = argparse.ArgumentParser(description="Direct RTMP publish to RTSP fanout benchmark")
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--server-bin", type=Path, default=Path("build/media_server"))
    parser.add_argument("--client-bin", type=Path, default=Path("build/rtsp_fanout"))
    parser.add_argument("--ffmpeg-bin", type=Path, default=Path("/home/gyl/bin/ffmpeg"))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--viewers", type=int, required=True)
    parser.add_argument("--workers", type=int, default=6)
    parser.add_argument("--ramp-per-second", type=int, default=50)
    parser.add_argument("--warmup", type=int, default=5)
    parser.add_argument("--duration", type=int, default=20)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--rtmp-port", type=int, default=11935)
    parser.add_argument("--rtsp-port", type=int, default=18554)
    parser.add_argument("--http-port", type=int, default=18080)
    args = parser.parse_args()
    if min(args.viewers, args.workers, args.ramp_per_second, args.duration) < 1 or args.warmup < 0:
        parser.error("viewers, workers, ramp-per-second and duration must be positive")

    args.output.mkdir(parents=True, exist_ok=True)
    client_log_path = args.output / "client.log"
    with (args.output / "server.log").open("w") as server_log, (args.output / "publisher.log").open("w") as publisher_log, \
         client_log_path.open("w") as client_log, (args.output / "client-error.log").open("w") as client_error:
        server = subprocess.Popen(
            [str(args.server_bin), "--threads", str(args.workers), "--rtmp-port", str(args.rtmp_port),
             "--rtsp-port", str(args.rtsp_port), "--http-port", str(args.http_port), "--bind-address", args.host,
             "--webrtc-address", args.host], stdout=server_log, stderr=subprocess.STDOUT,
        )
        publisher = None
        client = None
        try:
            wait_for_listener(args.host, args.rtmp_port)
            publisher = subprocess.Popen(
                [str(args.ffmpeg_bin), "-hide_banner", "-loglevel", "error", "-stream_loop", "-1", "-re", "-i", str(args.fixture),
                 "-c", "copy", "-f", "flv", f"rtmp://{args.host}:{args.rtmp_port}/live/perf0"],
                stdout=publisher_log, stderr=subprocess.STDOUT,
            )
            wait_for_stream(args.host, args.http_port)
            client = subprocess.Popen(
                [str(args.client_bin), "--stream-name", "live/perf0", "--media-host", args.host,
                 "--media-port", str(args.rtsp_port), "--viewers", str(args.viewers),
                 "--ramp-per-second", str(args.ramp_per_second), "--warmup", str(args.warmup),
                 "--duration", str(args.duration)], stdout=client_log, stderr=client_error,
            )
            wait_for_phase(client_log_path, client, "measurement_start", max(90, args.viewers / 10 + args.warmup))
            before_server = proc_snapshot(server.pid)
            samples = []
            client_samples = [proc_snapshot(client.pid)]
            started = time.monotonic()
            while time.monotonic() - started < args.duration:
                time.sleep(min(1, max(0, args.duration - (time.monotonic() - started))))
                samples.append(proc_snapshot(server.pid))
                if client.poll() is None:
                    try:
                        client_samples.append(proc_snapshot(client.pid))
                    except FileNotFoundError:
                        pass
            elapsed = time.monotonic() - started
            after_server = proc_snapshot(server.pid)
            measurement = parse_phase(wait_for_phase(client_log_path, client, "measurement", 30))
            client.wait(timeout=10)
            result = {
                "config": {
                    "head": benchmark_head(),
                    "fixture": str(args.fixture), "viewers": args.viewers, "workers": args.workers,
                    "ramp_per_second": args.ramp_per_second, "warmup_seconds": args.warmup,
                    "duration_seconds": args.duration, "kernel": platform.release(),
                },
                "measurement": measurement,
                "client_exit": client.returncode,
                "aggregate_gbit_per_second": 8 * (measurement["steady_video_bytes"] + measurement["steady_audio_bytes"]) /
                                             (1e9 * args.duration),
                "viewer_video_bytes_per_second": {
                    point: measurement[f"viewer_video_bytes_{point}"] / args.duration
                    for point in ("min", "p10", "p50", "p90", "max")
                },
                "viewer_audio_bytes_per_second": {
                    point: measurement[f"viewer_audio_bytes_{point}"] / args.duration
                    for point in ("min", "p10", "p50", "p90", "max")
                },
                "server_cpu_cores": (after_server["cpu"] - before_server["cpu"]) / elapsed,
                "server_rss_kib_median": statistics.median(sample["rss_kib"] for sample in samples),
                "server_pss_kib_median": statistics.median(sample["pss_kib"] for sample in samples),
                "server_fd_median": statistics.median(sample["fd"] for sample in samples),
                "server_thread_cpu_cores": thread_rates(before_server, after_server, "cpu", elapsed),
                "server_thread_context_switches_per_second": thread_rates(before_server, after_server, "context_switches", elapsed),
                "server_thread_migrations_per_second": thread_rates(before_server, after_server, "migrations", elapsed),
                "client_cpu_cores": measurement["client_cpu_cores"],
                "client_pss_kib_median": statistics.median(sample["pss_kib"] for sample in client_samples),
            }
            (args.output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
            print(json.dumps(result, indent=2))
            if client.returncode != 0 or measurement["progressing"] != args.viewers:
                raise SystemExit(1)
        finally:
            stop_process(client)
            stop_process(publisher)
            stop_process(server)


if __name__ == "__main__":
    main()
