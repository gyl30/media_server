#!/usr/bin/env python3

import argparse
import json
import platform
import socket
import statistics
import subprocess
import time
from pathlib import Path

from fanout_support import benchmark_head, parse_phase, proc_snapshot, stop_process, thread_rates, wait_for_listener, wait_for_phase


def source_has_media(host, port, name):
    try:
        with socket.create_connection((host, port), timeout=2) as connection:
            connection.settimeout(2)
            connection.sendall(f"GET /{name}.flv HTTP/1.1\r\nHost: {host}\r\n\r\n".encode())
            received = bytearray()
            while len(received) < 512:
                chunk = connection.recv(4096)
                if not chunk:
                    break
                received.extend(chunk)
                header_end = received.find(b"\r\n\r\n")
                if header_end >= 0 and received.startswith(b"HTTP/1.1 200") and len(received) - header_end > 64:
                    return True
    except OSError:
        pass
    return False


def wait_for_sources(host, port, names, publisher):
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        if publisher.poll() is not None:
            raise RuntimeError(f"WHIP publisher exited {publisher.returncode} before sources produced media")
        if source_has_media(host, port, names[0]):
            ready = True
            for name in names[1:]:
                try:
                    with socket.create_connection((host, port), timeout=2) as connection:
                        connection.sendall(f"GET /{name}.flv HTTP/1.1\r\nHost: {host}\r\n\r\n".encode())
                        if not connection.recv(64).startswith(b"HTTP/1.1 200"):
                            ready = False
                            break
                except OSError:
                    ready = False
                    break
            if ready:
                return
        time.sleep(0.2)
    raise RuntimeError("WHIP sources did not produce HTTP-FLV media")


def main():
    parser = argparse.ArgumentParser(description="Direct WHIP publish benchmark")
    parser.add_argument("--server-bin", type=Path, default=Path("build/media_server"))
    parser.add_argument("--client-bin", type=Path, default=Path("build/whip_publisher"))
    parser.add_argument("--output", type=Path, required=True)
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
    client_log_path = args.output / "publisher.log"
    with (args.output / "server.log").open("w") as server_log, client_log_path.open("w") as client_log:
        server = subprocess.Popen(
            [str(args.server_bin), "--threads", str(args.workers), "--rtmp-port", str(args.rtmp_port),
             "--rtsp-port", str(args.rtsp_port), "--http-port", str(args.http_port), "--bind-address", args.host,
             "--webrtc-address", args.host], stdout=server_log, stderr=subprocess.STDOUT,
        )
        publisher = None
        try:
            wait_for_listener(args.host, args.http_port)
            publisher = subprocess.Popen(
                ["stdbuf", "-oL", str(args.client_bin), "--whip-base-url", f"http://{args.host}:{args.http_port}/publish/whip/",
                 "--stream-prefix", "live/perf", "--sources", str(args.sources), "--io-threads", str(args.client_threads),
                 "--ramp-per-second", str(args.ramp_per_second), "--bitrate", str(args.bitrate),
                 "--duration", str(args.warmup + args.duration + 10)], stdout=client_log, stderr=subprocess.STDOUT,
            )
            names = [f"live/perf{index}" for index in range(args.sources)]
            wait_for_sources(args.host, args.http_port, names, publisher)
            established = parse_phase(wait_for_phase(client_log_path, publisher, "established", 5))
            time.sleep(args.warmup)
            before_server = proc_snapshot(server.pid)
            before_publisher = proc_snapshot(publisher.pid)
            samples = []
            started = time.monotonic()
            while time.monotonic() - started < args.duration:
                time.sleep(min(1, max(0, args.duration - (time.monotonic() - started))))
                samples.append(proc_snapshot(server.pid))
            elapsed = time.monotonic() - started
            after_server = proc_snapshot(server.pid)
            after_publisher = proc_snapshot(publisher.pid)
            publisher.wait(timeout=15)
            measurement = parse_phase(wait_for_phase(client_log_path, publisher, "measurement", 5))
            result = {
                "config": {
                    "head": benchmark_head(), "sources": args.sources, "workers": args.workers,
                    "client_threads": args.client_threads, "ramp_per_second": args.ramp_per_second,
                    "bitrate": args.bitrate, "warmup_seconds": args.warmup,
                    "duration_seconds": args.duration, "kernel": platform.release(),
                },
                "established": established,
                "measurement": measurement,
                "publisher_exit": publisher.returncode,
                "server_cpu_cores": (after_server["cpu"] - before_server["cpu"]) / elapsed,
                "server_pss_kib_median": statistics.median(sample["pss_kib"] for sample in samples),
                "server_fd_median": statistics.median(sample["fd"] for sample in samples),
                "server_thread_cpu_cores": thread_rates(before_server, after_server, "cpu", elapsed),
                "publisher_cpu_cores": (after_publisher["cpu"] - before_publisher["cpu"]) / elapsed,
                "publisher_pss_kib": after_publisher["pss_kib"],
            }
            (args.output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
            print(json.dumps(result, indent=2))
            if publisher.returncode != 0 or established["ready"] != args.sources or measurement["progressing"] != args.sources:
                raise SystemExit(1)
        finally:
            stop_process(publisher)
            stop_process(server)


if __name__ == "__main__":
    main()
