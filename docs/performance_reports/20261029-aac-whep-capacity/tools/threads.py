#!/usr/bin/env python3
"""Per-run server thread CPU distribution (top threads, cores) from result.json[.gz]."""
import gzip
import json
import sys
from pathlib import Path

for argument in sys.argv[1:]:
    run = Path(argument)
    plain, packed = run / "result.json", run / "result.json.gz"
    if not plain.exists() and not packed.exists():
        continue
    result = json.loads(plain.read_text() if plain.exists() else gzip.decompress(packed.read_bytes()))
    server = result.get("process_metrics", {}).get("server", {})
    threads = sorted(server.get("thread_cpu_cores", {}).values(), reverse=True)
    groups = result.get("groups", [])
    measured = [group["measurement"] for group in groups if group.get("measurement")]
    requested = sum(group.get("requested", 0) for group in groups)
    progressing = sum(item.get("progressing", 0) for item in measured)
    realtime = min((item.get("viewer_realtime_min", 0) for item in measured), default=0)
    lag = max((item.get("viewer_lag_span_ms_max", 0) for item in measured), default=0)
    cpu = result.get("cpu_busy", {})
    print(f"{run.name}: {result.get('status')}/{result.get('capacity_boundary')} {progressing}/{requested} rt={realtime:.3f} lag={lag} "
          f"Gcyc/s={cpu.get('server_gcycles_per_second', 0):.2f} drops={result.get('server_evidence', {}).get('udp_drops', '')} "
          f"threads=[{' '.join(f'{value:.2f}' for value in threads[:10])}] sum={sum(threads):.2f}")
