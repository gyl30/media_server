#!/usr/bin/env python3

import argparse
import json
import platform
import socket
import statistics
import subprocess
import time
from pathlib import Path

from fanout_support import benchmark_head, proc_snapshot, stop_process, thread_rates, wait_for_listener


def source_ready(host, port, name):
    try:
        with socket.create_connection((host, port), timeout=2) as connection:
            connection.sendall(f"GET /{name}.flv HTTP/1.1\r\nHost: {host}\r\n\r\n".encode())
            return connection.recv(64).startswith(b"HTTP/1.1 200")
    except OSError:
        return False


def wait_for_sources(host, port, names, publisher):
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline:
        if publisher.poll() is not None:
            raise RuntimeError(f"publisher exited {publisher.returncode} before sources became ready")
        if all(source_ready(host, port, name) for name in names):
            return
        time.sleep(0.2)
    raise RuntimeError("sources did not become readable")


def main():
    parser = argparse.ArgumentParser(description="Direct RTMP publish-only benchmark")
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--server-bin", type=Path, default=Path("build/media_server"))
    parser.add_argument("--ffmpeg-bin", type=Path, default=Path("/home/gyl/bin/ffmpeg"))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--sources", type=int, required=True)
    parser.add_argument("--workers", type=int, default=6)
    parser.add_argument("--warmup", type=int, default=5)
    parser.add_argument("--duration", type=int, default=20)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--rtmp-port", type=int, default=11935)
    parser.add_argument("--rtsp-port", type=int, default=18554)
    parser.add_argument("--http-port", type=int, default=18080)
    args = parser.parse_args()
    if min(args.sources, args.workers, args.duration) < 1 or args.warmup < 0:
        parser.error("sources, workers and duration must be positive; warmup must be nonnegative")

    args.output.mkdir(parents=True, exist_ok=True)
    names = [f"live/perf{index}" for index in range(args.sources)]
    addresses = [f"rtmp://{args.host}:{args.rtmp_port}/{name}" for name in names]
    output = addresses[0] if args.sources == 1 else "|".join(f"[f=flv]{address}" for address in addresses)
    format_name = "flv" if args.sources == 1 else "tee"
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
                 "-map", "0:v:0", "-map", "0:a:0", "-c", "copy", "-f", format_name, output],
                stdout=publisher_log, stderr=subprocess.STDOUT,
            )
            wait_for_sources(args.host, args.http_port, names, publisher)
            time.sleep(args.warmup)
            before_server = proc_snapshot(server.pid)
            before_publisher = proc_snapshot(publisher.pid)
            server_samples = []
            publisher_samples = []
            started = time.monotonic()
            while time.monotonic() - started < args.duration:
                time.sleep(min(1, max(0, args.duration - (time.monotonic() - started))))
                server_samples.append(proc_snapshot(server.pid))
                publisher_samples.append(proc_snapshot(publisher.pid))
            elapsed = time.monotonic() - started
            after_server = proc_snapshot(server.pid)
            after_publisher = proc_snapshot(publisher.pid)
            source_status = {name: source_ready(args.host, args.http_port, name) for name in names}
            result = {
                "config": {
                    "head": benchmark_head(),
                    "fixture": str(args.fixture), "sources": args.sources, "workers": args.workers,
                    "warmup_seconds": args.warmup, "duration_seconds": args.duration, "kernel": platform.release(),
                },
                "sources_readable": source_status,
                "publisher_alive": publisher.poll() is None,
                "server_cpu_cores": (after_server["cpu"] - before_server["cpu"]) / elapsed,
                "server_rss_kib_median": statistics.median(sample["rss_kib"] for sample in server_samples),
                "server_pss_kib_median": statistics.median(sample["pss_kib"] for sample in server_samples),
                "server_fd_median": statistics.median(sample["fd"] for sample in server_samples),
                "server_thread_cpu_cores": thread_rates(before_server, after_server, "cpu", elapsed),
                "server_thread_context_switches_per_second": thread_rates(before_server, after_server, "context_switches", elapsed),
                "server_thread_migrations_per_second": thread_rates(before_server, after_server, "migrations", elapsed),
                "publisher_cpu_cores": (after_publisher["cpu"] - before_publisher["cpu"]) / elapsed,
                "publisher_pss_kib_median": statistics.median(sample["pss_kib"] for sample in publisher_samples),
            }
            (args.output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
            print(json.dumps(result, indent=2))
            if not result["publisher_alive"] or not all(source_status.values()):
                raise SystemExit(1)
        finally:
            stop_process(publisher)
            stop_process(server)


if __name__ == "__main__":
    main()
