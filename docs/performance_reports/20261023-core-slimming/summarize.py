#!/usr/bin/env python3
import json
import sys
from pathlib import Path

runs = Path(sys.argv[1])
print("run\tstatus\tboundary\tserver_bin\toutputs\tprogressing\tcpu_cores\tpss_max_MiB\tqueue_full\tno_buffer_space\tudp_drops")
for result in sorted(runs.glob("*/result.json")):
    r = json.loads(result.read_text())
    c = r.get("config", {})
    pm = r.get("process_metrics", {}).get("server", {})
    ev = r.get("server_evidence", {})
    g = r.get("groups", [])
    progressing = f"{sum(x['measurement'].get('progressing') or 0 for x in g)}/{sum(x['requested'] for x in g)}"
    cpu = f"{pm['cpu_cores']:.4f}" if "cpu_cores" in pm else ""
    pss = f"{pm['pss_kib_max'] / 1024:.1f}" if "pss_kib_max" in pm else ""
    print("\t".join(str(v) for v in (result.parent.name, r["status"], r.get("capacity_boundary"), c.get("server_bin"), c.get("outputs"), progressing,
                                     cpu, pss, ev.get("queue_full", ""), ev.get("no_buffer_space", ""), ev.get("udp_drops", ""))))
