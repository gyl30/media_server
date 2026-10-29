#!/usr/bin/env python3
import csv
import json
import sys
from pathlib import Path

FIELDS = ("run", "status", "boundary", "input", "sources", "workers", "server_cpus", "outputs", "viewers", "server_busy", "server_user",
          "server_system", "server_softirq", "server_busy_mhz", "server_c0", "server_gcycles", "mcycles_per_viewer", "process_cores",
          "ginstr", "minstr_per_viewer", "ipc", "pss_mib", "fd_max", "client_busy", "client_busy_mhz", "runner_cores", "realtime_min", "lag_span_p50", "lag_span_max", "page_faults_per_s", "ctx_per_s", "window_s")


def row(path):
    result = json.loads((path / "result.json").read_text())
    config = result.get("config", {})
    busy = result.get("cpu_busy", {})
    groups = result.get("groups", [])
    viewers = sum(group["requested"] for group in groups)
    server = result.get("process_metrics", {}).get("server", {})
    realtime = [group["measurement"]["viewer_realtime_min"] for group in groups if "viewer_realtime_min" in group["measurement"]]
    outputs = {}
    for group in groups:
        protocol = group["name"].rsplit("-", 1)[0]
        outputs[protocol] = outputs.get(protocol, 0) + group["requested"]
    gcycles = busy.get("server_gcycles_per_second")
    window = result.get("requested_window", {})
    stat = result.get("perf_stat") or {}
    ginstr = stat["instructions"] / stat["seconds"] / 1e9 if stat.get("instructions") else None
    return {
        "run": path.name, "status": result.get("status"), "boundary": result.get("capacity_boundary"),
        "input": config.get("input"), "sources": config.get("sources"), "workers": config.get("workers"), "server_cpus": config.get("server_cpus"),
        "outputs": " ".join(f"{key}:{value}" for key, value in outputs.items()), "viewers": viewers,
        "server_busy": busy.get("server_busy"), "server_user": busy.get("server_user"), "server_system": busy.get("server_system"),
        "server_softirq": busy.get("server_softirq"), "server_busy_mhz": busy.get("server_busy_mhz"), "server_c0": busy.get("server_c0_residency"),
        "server_gcycles": gcycles, "mcycles_per_viewer": gcycles * 1000 / viewers if gcycles and viewers else None,
        "ginstr": ginstr, "minstr_per_viewer": ginstr * 1000 / viewers if ginstr and viewers else None, "ipc": stat.get("ipc"),
        "process_cores": server.get("cpu_cores"), "pss_mib": server.get("pss_kib_max", 0) / 1024 if server else None,
        "fd_max": server.get("fd_max"), "client_busy": busy.get("client_busy"), "client_busy_mhz": busy.get("client_busy_mhz"),
        "runner_cores": result.get("process_metrics", {}).get("runner", {}).get("cpu_cores"),
        "realtime_min": min(realtime) if realtime else None,
        "lag_span_p50": max((int(group["measurement"]["viewer_lag_span_ms_p50"]) for group in groups if "viewer_lag_span_ms_p50" in group["measurement"]), default=None),
        "lag_span_max": max((int(group["measurement"]["viewer_lag_span_ms_max"]) for group in groups if "viewer_lag_span_ms_max" in group["measurement"]), default=None),
        "page_faults_per_s": stat["page-faults"] / stat["seconds"] if "page-faults" in stat else None,
        "ctx_per_s": stat["context-switches"] / stat["seconds"] if "context-switches" in stat else None,
        "window_s": (window["end_monotonic_ns"] - window["start_monotonic_ns"]) / 1e9 if window else None,
    }


def main():
    writer = csv.DictWriter(sys.stdout, FIELDS)
    writer.writeheader()
    for item in sys.argv[1:]:
        path = Path(item)
        if not (path / "result.json").exists():
            continue
        values = row(path)
        writer.writerow({key: f"{value:.4g}" if isinstance(value, float) else value for key, value in values.items()})


if __name__ == "__main__":
    main()
