#!/usr/bin/env python3

import argparse
import json
import platform
import statistics
import subprocess
import time
from pathlib import Path

from fanout_support import benchmark_head, parse_phase, proc_snapshot, stop_process, thread_rates, wait_for_listener, wait_for_phase, wait_for_stream
from whip_publish import source_has_media


def main():
    parser = argparse.ArgumentParser(description="Direct RTSP multi-source publish benchmark")
    parser.add_argument("--server-bin", type=Path, default=Path("build/media_server"))
    parser.add_argument("--client-bin", type=Path, default=Path("build/rtsp_publisher"))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--transport", choices=("tcp", "udp"), required=True)
    parser.add_argument("--sources", type=int, required=True)
    parser.add_argument("--workers", type=int, default=6)
    parser.add_argument("--client-threads", type=int, default=8)
    parser.add_argument("--ramp-per-second", type=int, default=50)
    parser.add_argument("--bitrate", type=int, default=2_000_000)
    parser.add_argument("--warmup", type=int, default=3)
    parser.add_argument("--duration", type=int, default=15)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--rtmp-port", type=int, default=11935)
    parser.add_argument("--rtsp-port", type=int, default=18554)
    parser.add_argument("--http-port", type=int, default=18080)
    args = parser.parse_args()
    if min(args.sources, args.workers, args.client_threads, args.ramp_per_second, args.bitrate, args.duration) < 1 or args.warmup < 0:
        parser.error("sources, workers, client-threads, ramp, bitrate and duration must be positive")

    args.output.mkdir(parents=True, exist_ok=True)
    publisher_path = args.output / "publisher.log"
    with (args.output / "server.log").open("w") as server_log, publisher_path.open("w") as publisher_log:
        server = subprocess.Popen(
            [str(args.server_bin), "--threads", str(args.workers), "--rtmp-port", str(args.rtmp_port),
             "--rtsp-port", str(args.rtsp_port), "--http-port", str(args.http_port), "--bind-address", args.host,
             "--webrtc-address", args.host], stdout=server_log, stderr=subprocess.STDOUT,
        )
        publisher = None
        try:
            wait_for_listener(args.host, args.rtsp_port)
            publisher = subprocess.Popen(
                ["stdbuf", "-oL", str(args.client_bin), "--stream-prefix", "live/perf", "--media-host", args.host,
                 "--media-port", str(args.rtsp_port), "--transport", args.transport, "--sources", str(args.sources),
                 "--io-threads", str(args.client_threads), "--ramp-per-second", str(args.ramp_per_second),
                 "--bitrate", str(args.bitrate), "--warmup", str(args.warmup), "--duration", str(args.duration)],
                stdout=publisher_log, stderr=subprocess.STDOUT,
            )
            established = parse_phase(wait_for_phase(publisher_path, publisher, "established", max(35, args.sources / 10 + 10)))
            if established["ready"] != args.sources:
                raise RuntimeError(f"RTSP publish establishment failed: {publisher_path.read_text()}")
            names = ("live/perf/0", f"live/perf/{args.sources // 2}", f"live/perf/{args.sources - 1}")
            for name in names:
                wait_for_stream(args.host, args.http_port, name)
                if not source_has_media(args.host, args.http_port, name):
                    raise RuntimeError(f"RTSP source {name} has no readable media")
            wait_for_phase(publisher_path, publisher, "measurement_start", args.warmup + 5)
            before_server = proc_snapshot(server.pid)
            samples = []
            started = time.monotonic()
            while time.monotonic() - started < args.duration:
                time.sleep(min(1, max(0, args.duration - (time.monotonic() - started))))
                samples.append(proc_snapshot(server.pid))
            elapsed = time.monotonic() - started
            after_server = proc_snapshot(server.pid)
            measurement = parse_phase(wait_for_phase(publisher_path, publisher, "measurement", 10))
            fairness = parse_phase(wait_for_phase(publisher_path, publisher, "fairness", 10))
            publisher.wait(timeout=10)
            result = {
                "config": {
                    "head": benchmark_head(), "transport": args.transport, "sources": args.sources,
                    "workers": args.workers, "client_threads": args.client_threads,
                    "ramp_per_second": args.ramp_per_second, "bitrate": args.bitrate,
                    "warmup_seconds": args.warmup, "duration_seconds": args.duration,
                    "kernel": platform.release(),
                },
                "established": established,
                "measurement": measurement,
                "publisher_exit": publisher.returncode,
                "aggregate_gbit_per_second": 8 * measurement["sent_bytes"] / (1e9 * measurement["duration_seconds"]),
                "source_bytes_per_second": {
                    point: fairness[f"bytes_{point}"] / measurement["duration_seconds"]
                    for point in ("min", "p10", "p50", "p90", "max")
                },
                "server_cpu_cores": (after_server["cpu"] - before_server["cpu"]) / elapsed,
                "server_pss_kib_median": statistics.median(sample["pss_kib"] for sample in samples),
                "server_fd_median": statistics.median(sample["fd"] for sample in samples),
                "server_thread_cpu_cores": thread_rates(before_server, after_server, "cpu", elapsed),
                "publisher_cpu_cores": measurement["generator_cpu_cores"],
                "queue_full_events": (args.output / "server.log").read_text().count("write queue full"),
            }
            (args.output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
            print(json.dumps(result, indent=2))
            if publisher.returncode != 0 or measurement["progressing"] != args.sources or measurement["runtime_failures"]:
                raise SystemExit(1)
        finally:
            stop_process(publisher)
            stop_process(server)


if __name__ == "__main__":
    main()
