#!/usr/bin/env python3
import json
import re
import subprocess
import sys
from pathlib import Path

runs = Path(sys.argv[1])
scan = Path(__file__).with_name("scan_reports.sh")
for result in sorted(runs.glob(sys.argv[2] + "/result.json")):
    run = result.parent
    r = json.loads(result.read_text())
    server = [p["exit_code"] for p in r.get("processes", []) if p["name"] == "server"]
    g = r["groups"]
    progressing = f"{sum(x['measurement'].get('progressing') or 0 for x in g)}/{sum(x['requested'] for x in g)}"
    categories = sorted({x.get("failure_category") for x in g if x.get("failure_category")})
    dirs = [str(run)] + ([str(run) + ".san"] if Path(str(run) + ".san").exists() else [])
    out = subprocess.run([str(scan)] + dirs, capture_output=True, text=True).stdout
    diagnostics = sum(int(m) for m in re.findall(r"sanitizer_diagnostics=(\d+)", out))
    print(f"{run.name}\t{r['status']}\t{r.get('capacity_boundary')}\t{progressing}\tserver_exit={server}\tcategories={categories}\tsan={diagnostics}")
