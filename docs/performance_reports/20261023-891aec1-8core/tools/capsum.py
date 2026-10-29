#!/usr/bin/env python3
import json
import sys
from collections import defaultdict
from pathlib import Path


INVALID = Path(__file__).resolve().parent.parent / "analysis" / "invalid-rounds.txt"


def main():
    invalid = {line.split()[0] for line in INVALID.read_text().splitlines() if line.strip()}
    tally = defaultdict(lambda: defaultdict(list))
    for item in sys.argv[1:]:
        for result in sorted(Path(item).glob("round-*/result.json")):
            if "/".join(result.parent.parts[-3:]) in invalid:
                continue
            data = json.loads(result.read_text())
            groups = data.get("groups", [])
            protocols = sorted({group["name"].rsplit("-", 1)[0] for group in groups})
            viewers = sum(group["requested"] for group in groups)
            busy = data.get("cpu_busy", {}).get("server_busy")
            realtime = [group["measurement"]["viewer_realtime_min"] for group in groups if "viewer_realtime_min" in group["measurement"]]
            label = f"{data['status']}/{data.get('capacity_boundary')}"
            detail = f"{label} busy={busy:.2f}" if busy is not None else label
            if realtime:
                detail += f" rt={min(realtime):.3f}"
            tally["+".join(protocols)][viewers].append(f"{Path(item).name}:{result.parent.name[6:9]} {detail}")
    for protocol, counts in tally.items():
        print(f"== {protocol}")
        for viewers in sorted(counts):
            entries = counts[viewers]
            passed = sum(entry.split(" ")[1].startswith("PASS") for entry in entries)
            print(f"  {viewers:6d} pass {passed}/{len(entries)}  " + " | ".join(entries))


if __name__ == "__main__":
    main()
