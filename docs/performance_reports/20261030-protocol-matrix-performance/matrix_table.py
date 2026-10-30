#!/usr/bin/env python3
"""Print regression/protocol_matrix.py summary.json files as one markdown row per scenario; later files override earlier scenario rows."""

import argparse
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("summaries", nargs="+", type=Path)
    args = parser.parse_args()
    rows = {}
    for path in args.summaries:
        for name, summary in json.loads(path.read_text()).items():
            rows[name] = (path.parent.name, summary)
    print("| scenario | run | result | kind | groups ready/requested | A/V probe (offset p50 ms) | decode probe | FD base/after | receivers left |")
    print("|---|---|---|---|---|---|---|---|---|")
    for name, (run, s) in rows.items():
        groups = " ".join(f"{group}:{value['ready']}/{value['requested']}" for group, value in s.get("groups", {}).items())
        probes = " ".join(f"{protocol}:{'ok' if probe['ok'] else 'FAIL'}({probe.get('av_offset_ms_p50') or 0:.1f})"
                          for protocol, probe in s.get("av_probe", {}).items())
        decode = " ".join(f"{protocol}:{'ok' if probe['ok'] else 'FAIL'}" for protocol, probe in s.get("decode_probe", {}).items())
        kind = f"expected {s['expected_failure']}" if s.get("expected_failure") else "media"
        cleanup = s.get("cleanup", {})
        print(f"| {name} | {run} | {s.get('status')} | {kind} | {groups or '—'} | {probes or '—'} | {decode or '—'} | "
              f"{cleanup.get('fd_baseline')}/{cleanup.get('fd_after')} | {len(cleanup.get('remaining_receivers') or [])} |")


if __name__ == "__main__":
    main()
