#!/usr/bin/env python3
"""One line per perf_run directory: status, viewer realtime/lag, server cycles, drops and control probe."""
import json
import sys
from pathlib import Path

for argument in sys.argv[1:]:
    run = Path(argument)
    try:
        result = json.loads((run / "result.json").read_text())
    except (OSError, ValueError) as error:
        print(run.name, "ERROR", error)
        continue
    groups = result.get("groups", [])
    measured = [group["measurement"] for group in groups if group.get("measurement")]
    requested = sum(group.get("requested", 0) for group in groups)
    progressing = sum(item.get("progressing", 0) for item in measured)
    realtime = min((item.get("viewer_realtime_min", 0) for item in measured), default=0)
    lag = max((item.get("viewer_lag_span_ms_max", 0) for item in measured), default=0)
    cpu = result.get("cpu_busy", {})
    server = result.get("process_metrics", {}).get("server", {})
    upstream = result.get("process_metrics", {}).get("upstream", {})
    hot_upstream = max(upstream.get("thread_cpu_cores", {}).values(), default=0)
    probe = result.get("control_probe") or {}
    fields = [f"{run.name:24}", f"{result.get('status')}/{result.get('capacity_boundary')}", f"prog={progressing}/{requested}", f"rt_min={realtime:.3f}",
              f"lag_max={lag}", f"cores={server.get('cpu_cores', 0):.2f}", f"gcyc={cpu.get('server_gcycles_per_second', 0):.2f}",
              f"mhz={cpu.get('server_busy_mhz', 0):.0f}", f"maxcpu={cpu.get('server_max_cpu_busy', 0):.2f}", f"up_hot={hot_upstream:.2f}",
              f"ev={result.get('server_evidence', {})}"]
    if probe:
        fields.append(f"ctl_p50={probe.get('ms_p50', 0):.1f} ctl_p99={probe.get('ms_p99', 0):.1f} ctl_max={probe.get('ms_max', 0):.1f} ctl_err={probe.get('errors')}")
    for protocol, item in (result.get("av_probe") or {}).items():
        fields.append(f"av_{protocol}={item.get('ok')}:{item.get('offset_ms_min')}/{item.get('offset_ms_p50')}/{item.get('offset_ms_max')}")
    print(" ".join(fields))
