#!/usr/bin/env python3

import argparse
import json
import os
import platform
import selectors
import socket
import statistics
import subprocess
import time
import urllib.request
import uuid
from pathlib import Path

from fanout_support import benchmark_head, proc_snapshot, stop_process, thread_rates, wait_for_listener, wait_for_stream


def post(host, port, path, body):
    request = urllib.request.Request(
        f"http://{host}:{port}{path}", json.dumps(body).encode(),
        headers={"Content-Type": "application/json"}, method="POST",
    )
    with urllib.request.urlopen(request, timeout=5) as response:
        content = response.read()
        return json.loads(content) if content else {}


def create_pair(args, index):
    target = f"relay/perf{index}"
    receiver_id = str(uuid.uuid4())
    sender_id = str(uuid.uuid4())
    sender_name = f"bench{index}"
    ssrc = 100_000_000 + index
    receiver = {"stream_id": receiver_id, "stream_name": target, "payload_type": 96, "ssrc": ssrc}
    sender = {"stream_id": sender_id, "stream_name": "live/perf0", "sender_id": sender_name, "payload_type": 96, "ssrc": ssrc}
    if args.transport == "udp":
        receiver["transport"] = "udp"
        ports = post(args.host, args.http_port, "/gb28181/receiver/create", receiver)
        sender.update(transport="udp", remote_address=args.host, remote_rtp_port=ports["rtp_port"])
        post(args.host, args.http_port, "/gb28181/sender/create", sender)
    elif args.transport == "tcp_sender_active":
        port = args.tcp_port_base + index
        receiver.update(transport="tcp_passive", listen_port=port)
        sender.update(transport="tcp_active", remote_address=args.host, remote_port=port)
        post(args.host, args.http_port, "/gb28181/receiver/create", receiver)
        post(args.host, args.http_port, "/gb28181/sender/create", sender)
    else:
        port = args.tcp_port_base + index
        sender.update(transport="tcp_passive", listen_port=port)
        receiver.update(transport="tcp_active", remote_address=args.host, remote_port=port)
        post(args.host, args.http_port, "/gb28181/sender/create", sender)
        post(args.host, args.http_port, "/gb28181/receiver/create", receiver)
    return target, receiver_id, sender_id, sender_name


def open_viewer(host, port, name):
    connection = socket.create_connection((host, port), timeout=5)
    connection.settimeout(5)
    connection.sendall(f"GET /{name}.flv HTTP/1.1\r\nHost: {host}\r\n\r\n".encode())
    received = bytearray()
    while b"\r\n\r\n" not in received:
        data = connection.recv(4096)
        if not data:
            connection.close()
            raise RuntimeError(f"{name} closed before HTTP response")
        received.extend(data)
    if not received.startswith(b"HTTP/1.1 200"):
        connection.close()
        raise RuntimeError(f"{name} HTTP response was not 200")
    connection.setblocking(False)
    return connection


def percentiles(values):
    ordered = sorted(values)
    def at(fraction):
        position = (len(ordered) - 1) * fraction
        lower = int(position)
        upper = min(lower + 1, len(ordered) - 1)
        return ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)
    return {key: at(value) for key, value in (("min", 0), ("p10", .1), ("p50", .5), ("p90", .9), ("max", 1))}


def measure(args, server, publisher, receivers):
    selector = selectors.DefaultSelector()
    for index, connection in enumerate(receivers):
        selector.register(connection, selectors.EVENT_READ, index)
    received = [0] * len(receivers)
    errors = []
    started = time.monotonic()
    before_server = None
    before_client = None
    before_publisher = None
    samples = []
    next_sample = args.warmup + 1
    while time.monotonic() - started < args.warmup + args.duration:
        elapsed = time.monotonic() - started
        if elapsed >= args.warmup and before_server is None:
            before_server = proc_snapshot(server.pid)
            before_client = proc_snapshot(os.getpid())
            before_publisher = proc_snapshot(publisher.pid)
            received = [0] * len(receivers)
        for key, _ in selector.select(.1):
            try:
                data = key.fileobj.recv(65536)
                if not data:
                    errors.append(f"viewer {key.data} ended")
                    selector.unregister(key.fileobj)
                elif before_server is not None:
                    received[key.data] += len(data)
            except OSError as error:
                errors.append(f"viewer {key.data}: {error}")
                selector.unregister(key.fileobj)
        if before_server is not None and elapsed >= next_sample:
            samples.append(proc_snapshot(server.pid))
            next_sample += 1
    after_server = proc_snapshot(server.pid)
    after_client = proc_snapshot(os.getpid())
    after_publisher = proc_snapshot(publisher.pid)
    if not samples:
        samples.append(after_server)
    for connection in receivers:
        connection.close()
    selector.close()
    elapsed = time.monotonic() - started - args.warmup
    return {
        "ready": len(receivers), "progressing": sum(value / elapsed >= 10_000 for value in received),
        "errors": errors, "duration_seconds": elapsed,
        "aggregate_gbit_per_second": 8 * sum(received) / (1e9 * elapsed),
        "viewer_bytes_per_second": percentiles([value / elapsed for value in received]),
        "server_cpu_cores": (after_server["cpu"] - before_server["cpu"]) / elapsed,
        "server_pss_kib_median": statistics.median(sample["pss_kib"] for sample in samples),
        "server_fd_median": statistics.median(sample["fd"] for sample in samples),
        "server_thread_cpu_cores": thread_rates(before_server, after_server, "cpu", elapsed),
        "client_cpu_cores": (after_client["cpu"] - before_client["cpu"]) / elapsed,
        "publisher_cpu_cores": (after_publisher["cpu"] - before_publisher["cpu"]) / elapsed,
    }


def main():
    parser = argparse.ArgumentParser(description="GB28181 sender to receiver UDP/TCP network benchmark")
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--server-bin", type=Path, default=Path("build/media_server"))
    parser.add_argument("--ffmpeg-bin", type=Path, default=Path("bin/ffmpeg"))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--transport", choices=("udp", "tcp_sender_active", "tcp_sender_passive"), required=True)
    parser.add_argument("--pairs", type=int, required=True)
    parser.add_argument("--workers", type=int, default=6)
    parser.add_argument("--warmup", type=int, default=3)
    parser.add_argument("--duration", type=int, default=15)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--rtmp-port", type=int, default=11935)
    parser.add_argument("--rtsp-port", type=int, default=18554)
    parser.add_argument("--http-port", type=int, default=18080)
    parser.add_argument("--tcp-port-base", type=int, default=31000)
    args = parser.parse_args()
    if min(args.pairs, args.workers, args.duration) < 1 or args.warmup < 0 or args.pairs > 1000:
        parser.error("pairs, workers and duration must be positive; pairs at most 1000")

    args.output.mkdir(parents=True, exist_ok=True)
    with (args.output / "server.log").open("w") as server_log, (args.output / "publisher.log").open("w") as publisher_log:
        server = subprocess.Popen(
            [str(args.server_bin), "--threads", str(args.workers), "--rtmp-port", str(args.rtmp_port),
             "--rtsp-port", str(args.rtsp_port), "--http-port", str(args.http_port), "--bind-address", args.host,
             "--webrtc-address", args.host], stdout=server_log, stderr=subprocess.STDOUT,
        )
        publisher = None
        pairs = []
        try:
            wait_for_listener(args.host, args.rtmp_port)
            publisher = subprocess.Popen(
                [str(args.ffmpeg_bin), "-hide_banner", "-loglevel", "error", "-stream_loop", "-1", "-re", "-i", str(args.fixture),
                 "-c", "copy", "-f", "flv", f"rtmp://{args.host}:{args.rtmp_port}/live/perf0"],
                stdout=publisher_log, stderr=subprocess.STDOUT,
            )
            wait_for_stream(args.host, args.http_port)
            for index in range(args.pairs):
                pairs.append(create_pair(args, index))
            for name, _, _, _ in pairs:
                wait_for_stream(args.host, args.http_port, name)
            receivers = [open_viewer(args.host, args.http_port, pair[0]) for pair in pairs]
            result = measure(args, server, publisher, receivers)
            result["config"] = {
                "head": benchmark_head(), "fixture": str(args.fixture), "transport": args.transport,
                "pairs": args.pairs, "workers": args.workers, "warmup_seconds": args.warmup,
                "duration_seconds": args.duration, "kernel": platform.release(),
            }
            result["publisher_alive"] = publisher.poll() is None
            result["queue_full_events"] = (args.output / "server.log").read_text().count("write queue full")
            (args.output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
            print(json.dumps(result, indent=2))
            if result["progressing"] != args.pairs or result["errors"] or not result["publisher_alive"]:
                raise SystemExit(1)
        finally:
            for name, receiver_id, sender_id, sender_name in pairs:
                try:
                    post(args.host, args.http_port, "/gb28181/sender/delete", {"stream_id": sender_id, "stream_name": "live/perf0", "sender_id": sender_name})
                    post(args.host, args.http_port, "/gb28181/receiver/delete", {"stream_id": receiver_id, "stream_name": name})
                except Exception:
                    pass
            stop_process(publisher)
            stop_process(server)


if __name__ == "__main__":
    main()
