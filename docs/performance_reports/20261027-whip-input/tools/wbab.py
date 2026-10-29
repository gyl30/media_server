#!/usr/bin/env python3
import json
import re
import sys
from collections import defaultdict
from pathlib import Path

rows = defaultdict(list)
for path in sorted(map(Path, sys.argv[1:])):
    match = re.match(r"wb-whip(\d+)-(\w+)-r(\d+)$", path.name)
    result = path / "result.json"
    if not match or not result.exists():
        continue
    data = json.loads(result.read_text())
    sources, variant = int(match.group(1)), match.group(2)
    busy = data.get("cpu_busy") or {}
    threads = sorted((data.get("process_metrics") or {}).get("server", {}).get("thread_cpu_cores", {}).values(), reverse=True)[:8]
    stat = data.get("perf_stat") or {}
    measurement = [group.get("measurement", {}) for group in data.get("groups", [])]
    realtime = [m.get("viewer_realtime_min") for m in measurement if m.get("viewer_realtime_min") is not None]
    lag = [m.get("viewer_lag_span_ms_max") for m in measurement if m.get("viewer_lag_span_ms_max") is not None]
    rows[(sources, variant)].append(data["status"])
    print(f"{path.name:22} {data['status']:5} {data['capacity_boundary']:16} "
          f"busy={busy.get('server_busy', 0):5.2f} maxcpu={busy.get('server_max_cpu_busy', 0):5.3f} "
          f"thr={threads[0] if threads else 0:5.3f}/{threads[-1] if threads else 0:5.3f} "
          f"Gcyc/s={busy.get('server_gcycles_per_second', 0):5.2f} MHz={busy.get('server_busy_mhz', 0):5.0f} "
          f"Minst/src={stat.get('instructions', 0) / max(stat.get('seconds', 1), 1) / sources / 1e6:6.1f} "
          f"drops={(data.get('server_evidence') or {}).get('udp_drops', '-')} "
          f"rt_min={min(realtime) if realtime else '-'} lag_max={max(lag) if lag else '-'}")
print()
for (sources, variant), statuses in sorted(rows.items()):
    print(f"{sources} {variant}: {statuses.count('PASS')}/{len(statuses)} PASS")
