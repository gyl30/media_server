#!/usr/bin/env python3
import re
import sys
from pathlib import Path


def histogram(lines, start):
    buckets = []
    for line in lines[start + 1:]:
        match = re.match(r"\[(\d+)([KM]?)(?:, [\dKM]+\))?\]?\s+(\d+)", line)
        if not match:
            break
        buckets.append((int(match.group(1)) * {"": 1, "K": 1024, "M": 1024 * 1024}[match.group(2)], int(match.group(3))))
    return buckets


def summarize(path):
    path = Path(path)
    lines = (path / "drain.txt").read_text().splitlines()
    out = [path.name]
    for kind in ("media", "ps", "flv"):
        values = {}
        for index, line in enumerate(lines):
            match = re.match(rf"@{kind}_(n|max_us|batch_max_us): (\d+)", line)
            if match:
                values[match.group(1)] = int(match.group(2))
            if line.startswith(f"@{kind}_iters:"):
                buckets = histogram(lines, index)
                total = sum(count for _, count in buckets)
                multi = sum(count for low, count in buckets if low >= 2)
                values["multi"] = multi / total if total else 0
            if line.startswith(f"@{kind}_us:"):
                buckets = histogram(lines, index)
                total = sum(count for _, count in buckets)
                values["over5ms"] = sum(count for low, count in buckets if low >= 8192)
                values["over100ms"] = sum(count for low, count in buckets if low >= 131072)
        if values.get("n"):
            out.append(f"{kind}: handlers={values['n']} max={values.get('max_us', 0) / 1000:.1f}ms "
                       f"multi_frame={values.get('multi', 0) * 100:.0f}% >=8ms={values.get('over5ms', 0)} >=131ms={values.get('over100ms', 0)}")
    times = []
    timeouts = 0
    for line in (path / "probe.txt").read_text().splitlines():
        match = re.search(r"http=\[(\d+) [\d.]+ ([\d.]+)\]", line)
        if not match:
            continue
        if match.group(1) == "000":
            timeouts += 1
        times.append(float(match.group(2)) * 1000)
    times.sort()
    if times:
        pick = lambda q: times[min(len(times) - 1, int(len(times) * q))]
        out.append(f"curl n={len(times)} p50={pick(0.5):.0f} p90={pick(0.9):.0f} p99={pick(0.99):.0f} max={times[-1]:.0f} ms timeouts={timeouts}")
    print("  ".join(out))


for argument in sys.argv[1:]:
    summarize(argument)
