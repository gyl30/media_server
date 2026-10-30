#!/usr/bin/env python3
"""Print one markdown row per perf_run result directory (result.json or result.json.gz) for the mixed workload report."""

import argparse
import gzip
import json
from pathlib import Path


def load(path):
    gz = path / "result.json.gz"
    return json.load(gzip.open(gz)) if gz.exists() else json.loads((path / "result.json").read_text())


def quantile(values, fraction):
    return values[min(len(values) - 1, int(len(values) * fraction))] if values else None


def fmt(value, digits=0):
    return "—" if value is None else f"{value:.{digits}f}"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("runs", nargs="+", type=Path)
    parser.add_argument("--table", choices=["capacity", "resources", "quality"], default="capacity")
    args = parser.parse_args()
    headers = {
        "capacity": "| run | status | inputs | outputs | Opus→AAC | in/out Mbit/s | busy cores | busiest core | worker threads | Gcycles/s | Ginstr/s | IPC |",
        "resources": "| run | PSS MiB | FD | threads | UDP drops | realtime min | lag max ms | first frame p99 max ms | control p50/p95/p99 ms | source churn ready ms (publish/pull) | cleanup FD/PSS MiB/remaining |",
        "quality": "| run | output | ok | video frames | audio gaps (max ms) | video gaps | decode warnings | video pts backward | A/V offset ms |",
    }
    print(headers[args.table])
    print("|" + "---|" * (headers[args.table].count("|") - 1))
    for path in args.runs:
        r = load(path)
        config = r["config"]
        inputs = config.get("input_counts") or {config["input"]: config["sources"]}
        outputs = config["counts"]
        if args.table == "quality":
            for name, probe in r.get("av_probe", {}).items():
                if not isinstance(probe, dict):
                    continue
                offset = (probe.get("av_offset_ms_min"), probe.get("av_offset_ms_max"))
                print(f"| {path.name} | {name} | {probe.get('ok')} | {probe.get('video_frames')} | {probe.get('audio_pts_gaps')} ({fmt(probe.get('audio_pts_gap_max_ms'))}) | "
                      f"{probe.get('video_pts_gaps')} | {probe.get('decode_warnings')} | {probe.get('video_pts_backward')} | "
                      f"{'—' if offset[0] is None else f'{offset[0]:.1f}…{offset[1]:.1f}'} |")
            continue
        metrics = r.get("process_metrics", {}).get("server", {})
        busy = r.get("cpu_busy", {})
        if args.table == "capacity":
            fixture_bits = int(r.get("fixture_metadata", {}).get("format", {}).get("bit_rate", 0))
            stat = r.get("perf_stat", {})
            seconds = max(stat.get("seconds", 1), 1)
            threads = sorted((v for v in metrics.get("thread_cpu_cores", {}).values() if v > 0.01), reverse=True)
            print(f"| {path.name} | {r['status']} | {' '.join(f'{k}:{v}' for k, v in inputs.items())} | {' '.join(f'{k}:{v}' for k, v in outputs.items())} | "
                  f"{inputs.get('whip', 0) + inputs.get('whep_pull', 0)} | {sum(inputs.values()) * fixture_bits / 1e6:.0f}/{sum(outputs.values()) * fixture_bits / 1e6:.0f} | "
                  f"{busy.get('server_busy', 0):.2f} | {busy.get('server_max_cpu_busy', 0):.2f} | {' '.join(f'{v:.2f}' for v in threads)} | "
                  f"{stat.get('cycles', 0) / 1e9 / seconds:.2f} | {stat.get('instructions', 0) / 1e9 / seconds:.2f} | {stat.get('ipc') or 0:.2f} |")
            continue
        groups = r["groups"]
        realtime = [float(g["measurement"]["viewer_realtime_min"]) for g in groups if "viewer_realtime_min" in g["measurement"]]
        lag = [int(g["measurement"]["viewer_lag_span_ms_max"]) for g in groups if "viewer_lag_span_ms_max" in g["measurement"]]
        first = [float(g["first_media"].get("p99", 0)) for g in groups if g.get("first_media")]
        latency = sorted(s["ms"] for s in r.get("control_probe", {}).get("samples", []))
        samples = [s["server"] for s in r["samples"] if "server" in s and "threads" in s["server"]]
        rounds = r.get("source_churn", {}).get("rounds", [])
        cleanup = r.get("cleanup", {})
        settled = cleanup.get("server_settled", {})
        print(f"| {path.name} | {metrics.get('pss_kib_max', 0) / 1024:.0f} | {metrics.get('fd_max', 0)} | {max((len(s['threads']) for s in samples), default=0)} | "
              f"{r.get('server_evidence', {}).get('udp_drops')} | {fmt(min(realtime) if realtime else None, 4)} | {fmt(max(lag) if lag else None)} | "
              f"{fmt(max(first) if first else None)} | {fmt(quantile(latency, .5), 1)}/{fmt(quantile(latency, .95), 1)}/{fmt(quantile(latency, .99), 1)} | "
              f"{fmt(max((x.get('publish_ready_ms') or 0) for x in rounds) if rounds else None)}/{fmt(max((x.get('pull_ready_ms') or 0) for x in rounds) if rounds else None)} | "
              f"{settled.get('fd')}/{settled.get('pss_kib', 0) / 1024:.0f}/{len(cleanup.get('remaining_owned_receivers') or [])} |")


if __name__ == "__main__":
    main()
