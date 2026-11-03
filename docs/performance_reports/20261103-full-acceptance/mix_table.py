#!/usr/bin/env python3
"""Tabulate mixed-workload runs (perf_run result.json or result.json.gz). The verdict is the runner's status, except that a run failing only
because GB A/V probes report the load fixture's single loop-seam audio gap (one slip of at most 34 ms, also seen on FLV/RTMP/HLS) and no
sync markers (the load fixture has none) is shown as PASS(gb-probe): every other check, including UDP drops, readiness, realtime ratio,
churn, decode and cleanup, must pass. Columns: Gcycles/s and Ginstr/s from perf stat, busiest server core, worker thread max/min, minimum
realtime ratio, maximum lag span, server PSS and FD maxima, control p50/p99, UDP receive-buffer errors and settled FD."""

import gzip
import json
import sys
from pathlib import Path


def load(run):
    path = run / "result.json"
    return json.loads(path.read_text()) if path.exists() else json.load(gzip.open(run / "result.json.gz"))


def gb_probe_only(result):
    probes = result.get("av_probe", {})
    failed = {name: probe for name, probe in probes.items() if not probe.get("ok")}
    if not failed or any(not name.startswith("gb_") for name in failed):
        return False
    for probe in failed.values():
        checks = probe.get("checks", {})
        if {name for name, ok in checks.items() if not ok} - {"audio_timeline", "av_markers"}:
            return False
        if probe.get("audio_pts_overlaps") or probe.get("audio_pts_gaps", 0) > 1 or (probe.get("audio_pts_slip_ms_max") or 0) > 34:
            return False
    groups = result.get("groups", [])
    healthy = all(group.get("exit_code") == 0 and group["measurement"].get("ready") == group["requested"] and
                  group["measurement"].get("progressing") == group["requested"] for group in groups)
    realtime = min((float(group["measurement"]["viewer_realtime_min"]) for group in groups if "viewer_realtime_min" in group["measurement"]), default=0)
    return (healthy and not result.get("errors") and realtime >= 0.98 and result.get("server_evidence", {}).get("udp_drops") == 0 and
            result.get("namespace_tcp_delta", {}).get("UdpRcvbufErrors") == 0 and result.get("churn", {}).get("failed_rounds", 0) == 0 and
            result.get("source_churn", {}).get("failed_rounds", 0) == 0 and all(probe.get("ok") for probe in result.get("decode_probe", {}).values()) and
            not result.get("cleanup", {}).get("remaining_owned_receivers"))


def row(run):
    result = load(run)
    server = result.get("process_metrics", {}).get("server", {})
    threads = sorted((value for value in server.get("thread_cpu_cores", {}).values() if value > 0.05), reverse=True)
    groups = result.get("groups", [])
    stat = result.get("perf_stat", {})
    control = result.get("control_probe", {})
    verdict = result.get("status")
    if verdict != "PASS" and gb_probe_only(result):
        verdict = "PASS(gb-probe)"
    whep = result.get("av_probe", {}).get("whep", {})
    return [run.name, verdict, result.get("capacity_boundary"),
            f"{stat.get('cycles', 0) / 1e9 / max(stat.get('seconds', 1), 1):.2f}", f"{stat.get('instructions', 0) / 1e9 / max(stat.get('seconds', 1), 1):.2f}",
            f"{result.get('cpu_busy', {}).get('server_max_cpu_busy', 0):.3f}", f"{threads[0]:.2f}/{threads[-1]:.2f}" if threads else "-",
            f"{min((float(g['measurement']['viewer_realtime_min']) for g in groups if 'viewer_realtime_min' in g['measurement']), default=0):.4f}",
            str(max((int(g["measurement"]["viewer_lag_span_ms_max"]) for g in groups if "viewer_lag_span_ms_max" in g["measurement"]), default=0)),
            f"{server.get('pss_kib_max', 0) / 1024:.0f}", str(server.get("fd_max")),
            f"{control.get('ms_p50', 0):.1f}/{control.get('ms_p99', 0):.1f}", str(result.get("namespace_tcp_delta", {}).get("UdpRcvbufErrors")),
            f"{whep.get('audio_pts_gaps')}/{whep.get('decode_warnings')}", str((result.get("cleanup", {}).get("server_settled") or {}).get("fd"))]


HEADER = ["run", "verdict", "boundary", "Gcyc/s", "Ginstr/s", "busiest", "worker max/min", "realtime", "lag ms", "PSS MiB", "FD", "ctl p50/p99",
          "udp err", "whep gaps/warn", "fd after"]
print("| " + " | ".join(HEADER) + " |")
print("|" + " --- |" * len(HEADER))
for item in sys.argv[1:]:
    print("| " + " | ".join(row(Path(item))) + " |")
