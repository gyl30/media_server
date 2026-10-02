#!/usr/bin/env python3
"""Repeat existing benchmarks with fixed windows and report run medians."""

import argparse
import hashlib
import json
import statistics
import subprocess
import sys
from pathlib import Path

from fanout_support import benchmark_head, parse_phase


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--warmup", type=int, default=10)
    parser.add_argument("--duration", type=int, default=30)
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--whep-levels", type=int, nargs="+", default=[1, 100, 250])
    parser.add_argument("--workloads", nargs="+")
    args = parser.parse_args()
    if args.runs < 3 or args.duration < 1 or args.warmup < 0:
        parser.error("at least three runs, positive duration and nonnegative warmup are required")
    workloads = [("publish_only", 1)] + [(protocol, viewers) for protocol in ("rtsp", "rtmp", "http_flv", "hls")
                                          for viewers in (1, 4, 8)] + [("whep", viewers) for viewers in args.whep_levels]
    if args.workloads:
        workloads = [(protocol, viewers) for protocol, viewers in workloads if f"{protocol}-{viewers}" in args.workloads]
        if len(workloads) != len(args.workloads):
            parser.error("unknown or duplicate workload")
    args.output.mkdir(parents=True, exist_ok=True)
    report = {"head": benchmark_head(), "fixture_sha256": hashlib.sha256(args.fixture.read_bytes()).hexdigest(),
              "warmup_seconds": args.warmup, "duration_seconds": args.duration, "runs": args.runs, "workloads": {}}
    for protocol, viewers in workloads:
        label = f"{protocol}-{viewers}"
        rows = []
        for run in range(args.runs):
            output = args.output / label / str(run + 1)
            output.mkdir(parents=True, exist_ok=True)
            script = "publish_only.py" if protocol == "publish_only" else f"{protocol}_fanout.py"
            command = [sys.executable, str(Path(__file__).with_name(script)), "--fixture", str(args.fixture),
                       "--server-bin", str(args.build_dir / "media_server"), "--output", str(output),
                       "--warmup", str(args.warmup), "--duration", str(args.duration), "--workers", "6"]
            command += ["--sources", "1"] if protocol == "publish_only" else ["--viewers", str(viewers)]
            if protocol in ("rtmp", "rtsp", "whep"):
                command += ["--client-bin", str(args.build_dir / f"{protocol}_fanout")]
            (output / "command.json").write_text(json.dumps(command, indent=2) + "\n")
            with (output / "runner.log").open("w") as log:
                completed = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=args.warmup + args.duration + viewers / 10 + 120)
            if completed.returncode:
                raise RuntimeError(f"{label} run {run + 1} failed: {output / 'runner.log'}")
            data = json.loads((output / "result.json").read_text())
            if protocol in ("rtmp", "rtsp", "hls"):
                latency = parse_phase(next(line for line in (output / "client.log").read_text().splitlines()
                                           if line.startswith("first_media_ms_p50=")))["first_media_ms_p50"]
            elif protocol == "whep":
                latency = data["first_media"]["first_media_ms_p50"]
            elif protocol == "http_flv":
                latency = data["first_media_ms_p50"]
            else:
                latency = data["publish_first_media_ms"]
            throughput = data["published_bytes_per_second"] * 8 / 1e6 if protocol == "publish_only" else data["aggregate_gbit_per_second"] * 1000
            if protocol == "publish_only":
                failures = sum(not value for value in data["sources_readable"].values()) + int(not data["publisher_alive"])
            elif protocol == "http_flv":
                failures = len(data["errors"])
            elif protocol == "whep":
                failures = (data["established"]["establishment_failures"] + data["measurement"]["runtime_failures"]
                            + data["measurement"]["unprotect_failures"] + data["queue_full_events"] + data["udp_drops"])
            else:
                failures = data["measurement"]["failed"]
            assert failures == 0 and throughput > 0 and latency >= 0, (label, data)
            rows.append({"cpu_cores": data["server_cpu_cores"], "rss_kib": data["server_rss_kib_median"],
                         "mbit_per_second": throughput, "first_media_ms_p50": latency, "failures": failures})
            print(f"{label} run={run + 1} {json.dumps(rows[-1])}", flush=True)
        report["workloads"][label] = {"runs": rows, "median": {key: statistics.median(row[key] for row in rows) for key in rows[0]}}
        (args.output / "summary.json").write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
