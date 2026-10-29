#!/usr/bin/env python3
import re
import sys
from pathlib import Path
from statistics import mean

import ab

SPEC = [item.split("=", 1) if "=" in item else [item, item] for item in sys.argv[2].split(",")]
KEYS = [key for key, _ in SPEC]
HEADERS = [header for _, header in SPEC]


def fmt(values):
    values = [value for value in values if value is not None]
    if not values:
        return "—"
    if all(isinstance(value, str) for value in values):
        return " / ".join(values)
    numbers = [float(value) for value in values]
    show = lambda value: f"{value:.0f}" if abs(value) >= 100 else f"{value:.3g}"
    if len(numbers) == 1:
        return show(numbers[0])
    return f"{show(mean(numbers))} ({show(min(numbers))}～{show(max(numbers))})"


def main():
    prefixes = tuple(sys.argv[4].split(",")) if len(sys.argv) > 4 else ("ab-", "cost-")
    runs = sorted(path for path in Path(sys.argv[1]).iterdir() if path.name.startswith(prefixes))
    points = {}
    for run in runs:
        if not (run / "result.json").exists():
            continue
        match = re.match(r"[a-z0-9]+-(.+)-(A|B|B3s5|B3s20|B3)-r(\d+)$", run.name)
        if not match:
            continue
        points.setdefault(match.group(1), {}).setdefault(match.group(2), []).append(ab.row(run))
    selected = sys.argv[3].split(",") if len(sys.argv) > 3 else sorted(points)
    labels = dict(item.split("=") for item in sys.argv[5].split(",")) if len(sys.argv) > 5 and sys.argv[5] else {}
    print("| 负载点 | 版本 | 轮数 | 判定 | " + " | ".join(HEADERS) + " |")
    print("| --- | --- | --- | --- | " + " | ".join("---" for _ in KEYS) + " |")
    for point in selected:
        for variant in ("A", "B", "B3s5", "B3s20", "B3"):
            rows = points.get(point, {}).get(variant, [])
            if not rows:
                continue
            status = " ".join(row["status"] for row in rows)
            print(f"| {point} | {labels.get(variant, variant)} | {len(rows)} | {status} | " + " | ".join(fmt([row.get(key) for row in rows]) for key in KEYS) + " |")


if __name__ == "__main__":
    main()
