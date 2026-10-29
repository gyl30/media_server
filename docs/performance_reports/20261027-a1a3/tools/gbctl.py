#!/usr/bin/env python3
import json
import re
import statistics
import sys
from pathlib import Path


def fields(line):
    return {key: float(value) for key, value in re.findall(r"(\w+)=([0-9.]+)", line)}


for path in map(Path, sys.argv[1:]):
    result = path / "result.json"
    if not result.exists():
        continue
    status = json.loads(result.read_text())["status"]
    control, first = [], []
    for log in path.glob("gb_tcp_*.log"):
        for line in log.read_text(errors="replace").splitlines():
            if line.startswith("phase=control "):
                control.append(fields(line))
            elif line.startswith("first_media_ms_p50="):
                first.append(fields(line.replace("first_media_ms_p50", "p50")))
    if not control:
        continue
    median = lambda items, key: statistics.median(item[key] for item in items)
    worst = lambda items, key: max(item[key] for item in items)
    print(f"{path.name}\t{status}\tgroups={len(control)}"
          f"\tcreate p50 med/max={median(control, 'create_ms_p50'):.0f}/{worst(control, 'create_ms_p50'):.0f}"
          f"\tp90 med/max={median(control, 'create_ms_p90'):.0f}/{worst(control, 'create_ms_p90'):.0f}"
          f"\tp99 med/max={median(control, 'create_ms_p99'):.0f}/{worst(control, 'create_ms_p99'):.0f}"
          f"\tfirst_media p50 med={median(first, 'p50'):.0f} p95 max={worst(first, 'p95'):.0f} p99 max={worst(first, 'p99'):.0f}")
