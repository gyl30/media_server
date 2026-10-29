#!/usr/bin/env python3
import json
import sys
from pathlib import Path


def summarize(path):
    run = Path(path)
    result = json.loads((run / "result.json").read_text())
    metrics = result.get("process_metrics", {})
    server = metrics.get("server", {})
    groups = result.get("groups", [])
    requested = sum(group["requested"] for group in groups)
    ready = sum(group["measurement"].get("ready", 0) for group in groups)
    progressing = sum(group["measurement"].get("progressing", 0) for group in groups)
    clients = {name: value for name, value in metrics.items() if name not in ("server", "runner")}
    client_cores = sum(value["cpu_cores"] for value in clients.values())
    hot = max((max(value["thread_cpu_cores"].values(), default=0) for value in clients.values()), default=0)
    runner = metrics.get("runner", {}).get("cpu_cores", 0)
    line = (f"{run.name} {result['status']} {result.get('capacity_boundary')} server_cores={server.get('cpu_cores', 0):.4f} "
            f"pss_max_mib={server.get('pss_kib_max', 0) / 1024:.1f} fd_max={server.get('fd_max', 0)} ready={ready}/{requested} "
            f"progressing={progressing}/{requested} client_cores={client_cores:.3f} client_hot_thread={hot:.3f} runner_cores={runner:.3f} "
            f"evidence={result.get('server_evidence', {})}")
    cpus = result.get("cpu_busy")
    if cpus:
        keys = ("server_busy", "server_softirq", "server_interval_busy_max", "server_max_cpu_busy", "sibling_busy", "client_busy",
                "client_max_cpu_busy", "server_busy_mhz", "server_c0_residency", "server_gcycles_per_second", "client_busy_mhz", "client_gcycles_per_second")
        line += " " + " ".join(f"{key}={cpus[key]:.3f}" for key in keys if isinstance(cpus.get(key), (int, float)))
    preparation = result.get("preparation_cpu")
    if preparation and result.get("status") != "PASS":
        line += " prep=" + ",".join(f"{key}={value:.2f}" for key, value in preparation.items())
    realtime = [group["measurement"].get("viewer_realtime_min") for group in groups if "viewer_realtime_min" in group["measurement"]]
    if realtime:
        line += f" realtime_min={min(float(value) for value in realtime):.4f}"
    spans = [(group["measurement"]["viewer_lag_span_ms_p50"], group["measurement"]["viewer_lag_span_ms_max"]) for group in groups if "viewer_lag_span_ms_max" in group["measurement"]]
    if spans:
        line += f" lag_span_ms_p50max={max(int(p50) for p50, _ in spans)} lag_span_ms_max={max(int(value) for _, value in spans)}"
    print(line)
    for error in result.get("errors", []):
        print("  error:", error.get("message"))
    for group in groups:
        if group.get("failure_category") or group.get("exit_code"):
            print("  group:", group["name"], group.get("exit_code"), group.get("failure_category"), group.get("first_errors", [])[:3])


for item in sys.argv[1:]:
    try:
        summarize(item)
    except (OSError, ValueError, KeyError) as error:
        print(item, "unreadable", error)
