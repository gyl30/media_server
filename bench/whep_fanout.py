#!/usr/bin/env python3

import argparse
import json
import platform
import signal
import socket
import statistics
import subprocess
import time
from pathlib import Path

from process_metrics import proc_snapshot, thread_rates


def wait_for_listener(host, port):
    for _ in range(100):
        try:
            with socket.create_connection((host, port), timeout=0.2):
                return
        except OSError:
            time.sleep(0.1)
    raise RuntimeError("media server did not open RTMP listener")


def wait_for_stream(host, port):
    for _ in range(100):
        try:
            with socket.create_connection((host, port), timeout=2) as connection:
                connection.sendall(f"GET /live/perf0.flv HTTP/1.1\r\nHost: {host}\r\n\r\n".encode())
                if connection.recv(64).startswith(b"HTTP/1.1 200"):
                    return
        except OSError:
            pass
        time.sleep(0.1)
    raise RuntimeError("source perf0 did not become readable")


def wait_for_phase(path, process, phase, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        for line in path.read_text().splitlines():
            if line.startswith(f"phase={phase} ") or line == f"phase={phase}":
                return line
        if process.poll() is not None:
            raise RuntimeError(f"WHEP client exited {process.returncode} before {phase}: {path.read_text()}")
        time.sleep(0.05)
    raise RuntimeError(f"WHEP client did not reach {phase} within {timeout}s: {path.read_text()}")


def parse_phase(line):
    values = {}
    for field in line.split():
        key, _, value = field.partition("=")
        if value:
            try:
                values[key] = float(value) if any(char in value for char in ".eE") else int(value)
            except ValueError:
                values[key] = value
    return values


def stop_process(process):
    if process is not None and process.poll() is None:
        process.send_signal(signal.SIGTERM)
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


def main():
    parser = argparse.ArgumentParser(description="Direct RTMP publish to WHEP fanout benchmark")
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--server-bin", type=Path, default=Path("build/media_server"))
    parser.add_argument("--client-bin", type=Path, default=Path("build/whep_fanout"))
    parser.add_argument("--ffmpeg-bin", type=Path, default=Path("/home/gyl/bin/ffmpeg"))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--viewers", type=int, required=True)
    parser.add_argument("--workers", type=int, default=6)
    parser.add_argument("--client-threads", type=int, default=8)
    parser.add_argument("--ramp-per-second", type=int, default=100)
    parser.add_argument("--warmup", type=int, default=5)
    parser.add_argument("--duration", type=int, default=20)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--rtmp-port", type=int, default=11935)
    parser.add_argument("--rtsp-port", type=int, default=18554)
    parser.add_argument("--http-port", type=int, default=18080)
    args = parser.parse_args()
    if min(args.viewers, args.workers, args.client_threads, args.ramp_per_second, args.duration) < 1 or args.warmup < 0:
        parser.error("viewers, workers, client-threads, ramp-per-second and duration must be positive")

    args.output.mkdir(parents=True, exist_ok=True)
    server_log_path = args.output / "server.log"
    client_log_path = args.output / "client.log"
    with server_log_path.open("w") as server_log, (args.output / "publisher.log").open("w") as publisher_log, \
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
                [str(args.client_bin), "--whep-url", f"http://{args.host}:{args.http_port}/play/whep/live/perf0",
                 "--viewers", str(args.viewers), "--io-threads", str(args.client_threads),
                 "--ramp-per-second", str(args.ramp_per_second), "--warmup", str(args.warmup),
                 "--duration", str(args.duration)], stdout=client_log, stderr=client_error,
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
                    "head": subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip(),
                    "fixture": str(args.fixture), "viewers": args.viewers, "workers": args.workers,
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
