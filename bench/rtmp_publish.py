#!/usr/bin/env python3

import argparse
import json
import platform
import selectors
import statistics
import subprocess
import time
from pathlib import Path

from fanout_support import benchmark_head, proc_snapshot, stop_process, wait_for_listener, wait_for_stream
from gb_network import open_viewer, percentiles
from whip_publish import source_has_media


def main():
    parser = argparse.ArgumentParser(description="Single FFmpeg process publishing multiple RTMP streams with tee")
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--server-bin", type=Path, default=Path("build/media_server"))
    parser.add_argument("--ffmpeg-bin", type=Path, default=Path("bin/ffmpeg"))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--sources", type=int, required=True)
    parser.add_argument("--workers", type=int, default=6)
    parser.add_argument("--warmup", type=int, default=3)
    parser.add_argument("--duration", type=int, default=15)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--rtmp-port", type=int, default=11935)
    parser.add_argument("--rtsp-port", type=int, default=18554)
    parser.add_argument("--http-port", type=int, default=18080)
    args = parser.parse_args()
    if min(args.sources, args.workers, args.duration) < 1 or args.warmup < 0:
        parser.error("sources, workers and duration must be positive")

    args.output.mkdir(parents=True, exist_ok=True)
    with (args.output / "server.log").open("w") as server_log, (args.output / "publisher.log").open("w") as publisher_log:
        server = subprocess.Popen(
            [str(args.server_bin), "--threads", str(args.workers), "--rtmp-port", str(args.rtmp_port),
             "--rtsp-port", str(args.rtsp_port), "--http-port", str(args.http_port), "--bind-address", args.host,
             "--webrtc-address", args.host], stdout=server_log, stderr=subprocess.STDOUT,
        )
        publisher = None
        probes = []
        selector = selectors.DefaultSelector()
        try:
            wait_for_listener(args.host, args.rtmp_port)
            targets = "|".join(f"[f=flv]rtmp://{args.host}:{args.rtmp_port}/live/perf{index}" for index in range(args.sources))
            publisher = subprocess.Popen(
                [str(args.ffmpeg_bin), "-hide_banner", "-loglevel", "error", "-stream_loop", "-1", "-re", "-i", str(args.fixture),
                 "-map", "0:v:0", "-map", "0:a:0", "-c", "copy", "-f", "tee", targets],
                stdout=publisher_log, stderr=subprocess.STDOUT,
            )
            wait_for_stream(args.host, args.http_port)
            names = [f"live/perf{index}" for index in range(args.sources)]
            for name in names:
                deadline = time.monotonic() + 10
                while not source_has_media(args.host, args.http_port, name):
                    if publisher.poll() is not None or time.monotonic() >= deadline:
                        raise RuntimeError(f"RTMP source {name} has no readable media")
                    time.sleep(.2)
            probe_names = list(dict.fromkeys((names[0], names[len(names) // 2], names[-1])))
            probes = [open_viewer(args.host, args.http_port, name) for name in probe_names]
            for index, probe in enumerate(probes):
                selector.register(probe, selectors.EVENT_READ, index)

            bytes_received = [0] * len(probes)
            before_server = None
            samples = []
            errors = []
            started = time.monotonic()
            next_sample = args.warmup + 1
            while time.monotonic() - started < args.warmup + args.duration:
                elapsed = time.monotonic() - started
                if elapsed >= args.warmup and before_server is None:
                    before_server = proc_snapshot(server.pid)
                    before_publisher = proc_snapshot(publisher.pid)
                    bytes_received = [0] * len(probes)
                for key, _ in selector.select(.1):
                    try:
                        data = key.fileobj.recv(65536)
                        if not data:
                            errors.append(f"probe {key.data} ended")
                            selector.unregister(key.fileobj)
                        elif before_server is not None:
                            bytes_received[key.data] += len(data)
                    except OSError as error:
                        errors.append(f"probe {key.data}: {error}")
                        selector.unregister(key.fileobj)
                if before_server is not None and elapsed >= next_sample:
                    samples.append(proc_snapshot(server.pid))
                    next_sample += 1
            after_server = proc_snapshot(server.pid)
            after_publisher = proc_snapshot(publisher.pid)
            if not samples:
                samples.append(after_server)
            elapsed = time.monotonic() - started - args.warmup
            result = {
                "config": {
                    "head": benchmark_head(), "fixture": str(args.fixture), "sources": args.sources,
                    "workers": args.workers, "warmup_seconds": args.warmup,
                    "duration_seconds": args.duration, "kernel": platform.release(),
                },
                "sources_readable": len(names), "probes_progressing": sum(value / elapsed >= 10_000 for value in bytes_received),
                "probe_errors": errors,
                "probe_bytes_per_second": percentiles([value / elapsed for value in bytes_received]),
                "server_cpu_cores": (after_server["cpu"] - before_server["cpu"]) / elapsed,
                "server_pss_kib_median": statistics.median(sample["pss_kib"] for sample in samples),
                "server_fd_median": statistics.median(sample["fd"] for sample in samples),
                "publisher_cpu_cores": (after_publisher["cpu"] - before_publisher["cpu"]) / elapsed,
                "publisher_alive": publisher.poll() is None,
                "queue_full_events": (args.output / "server.log").read_text().count("write queue full"),
            }
            (args.output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
            print(json.dumps(result, indent=2))
            if result["probes_progressing"] != len(probes) or errors or not result["publisher_alive"]:
                raise SystemExit(1)
        finally:
            selector.close()
            for probe in probes:
                probe.close()
            stop_process(publisher)
            stop_process(server)


if __name__ == "__main__":
    main()
