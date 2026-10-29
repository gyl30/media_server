#!/usr/bin/env python3
import json
import sys
from pathlib import Path

KEYS = ("requested", "connected", "ready", "progressing", "failed", "runtime_failures", "non_progressing_viewers")

for path in map(Path, sys.argv[1:]):
    result = path / "result.json"
    if not result.exists():
        continue
    data = json.loads(result.read_text())
    total = dict.fromkeys(KEYS, 0)
    for group in data.get("groups", []):
        for key in KEYS:
            total[key] += group.get("measurement", {}).get(key) or 0
    print(path.name, data["status"], " ".join(f"{key}={value}" for key, value in total.items()))
