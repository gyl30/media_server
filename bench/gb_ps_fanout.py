#!/usr/bin/env python3

import argparse
import json
import platform
import statistics
import subprocess
import time
from pathlib import Path

from fanout_support import benchmark_head, proc_snapshot, stop_process, thread_rates, wait_for_phase


def main():
    parser = argparse.ArgumentParser(description="Shared GB28181 PS preparation and RTP callback benchmark")
    parser.add_argument("--client-bin", type=Path, default=Path("build/gb_ps_fanout"))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--viewers", type=int, required=True)
    parser.add_argument("--duration", type=int, default=20)
    parser.add_argument("--frame-bytes", type=int, default=16000)
    args = parser.parse_args()
    if args.viewers < 1 or args.duration < 1 or args.frame_bytes < 100:
        parser.error("viewers and duration must be positive; frame bytes must be at least 100")

    args.output.mkdir(parents=True, exist_ok=True)
    log_path = args.output / "client.log"
    with log_path.open("w") as log:
        client = subprocess.Popen(
            [str(args.client_bin), str(args.viewers), str(args.duration), str(args.frame_bytes)],
            stdout=log, stderr=subprocess.STDOUT,
        )
        try:
            wait_for_phase(log_path, client, "measurement_start", 30)
            before = proc_snapshot(client.pid)
            samples = []
            started = time.monotonic()
            while client.poll() is None:
                time.sleep(0.5)
                if client.poll() is None:
                    try:
                        samples.append(proc_snapshot(client.pid))
                    except FileNotFoundError:
                        break
            elapsed = time.monotonic() - started
            client.wait(timeout=5)
            result_line = next(line for line in reversed(log_path.read_text().splitlines()) if line.startswith("{"))
            measurement = json.loads(result_line)
            result = {
                "config": {
                    "head": benchmark_head(),
                    "viewers": args.viewers, "duration_seconds": args.duration,
                    "frame_bytes": args.frame_bytes, "kernel": platform.release(),
                    "transport": "packet callback only; no UDP/TCP socket",
                },
                "measurement": measurement,
                "client_exit": client.returncode,
                "process_pss_kib_median": statistics.median(sample["pss_kib"] for sample in samples),
                "process_rss_kib_median": statistics.median(sample["rss_kib"] for sample in samples),
                "process_fd_median": statistics.median(sample["fd"] for sample in samples),
                "process_thread_cpu_cores": thread_rates(before, samples[-1], "cpu", elapsed),
                "process_thread_context_switches_per_second": thread_rates(before, samples[-1], "context_switches", elapsed),
                "process_thread_migrations_per_second": thread_rates(before, samples[-1], "migrations", elapsed),
            }
            (args.output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
            print(json.dumps(result, indent=2))
            if client.returncode != 0:
                raise SystemExit(1)
        finally:
            stop_process(client)


if __name__ == "__main__":
    main()
