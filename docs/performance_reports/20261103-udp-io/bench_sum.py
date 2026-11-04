#!/usr/bin/env python3
"""Summarize udp_batch_bench JSON lines: median cycles, user cycles and instructions per packet over the repetitions of each
configuration, syscalls per packet, receiver integrity (received/expected bytes, reordering, corruption, size mismatches), EAGAIN and
short sends, and send-to-receive latency. Usage: bench_sum.py results.jsonl"""

import json
import statistics
import sys
from collections import defaultdict


def main():
    rows = defaultdict(list)
    for line in open(sys.argv[1]):
        line = line.strip()
        if not line.startswith("{"):
            if line:
                print("non-result line:", line[:200])
            continue
        item = json.loads(line)
        key = (item["size"], item["last_size"], item["burst"], item["sockets"], item["endpoints_per_socket"], item["copy"], item["mode"])
        rows[key].append(item)
    print("| size/last | burst | sockets x endpoints | copy | mode | n | cycles/pkt | user cycles/pkt | instr/pkt | calls/pkt | vs sendto | "
          "integrity | EAGAIN/short | latency p50/p99 us |")
    print("|" + " --- |" * 14)
    baseline = {}
    for key in sorted(rows):
        items = rows[key]
        cycles = statistics.median(item["cycles_per_packet"] for item in items)
        if key[-1] == "sendto":
            baseline[key[:-1]] = cycles
    for key in sorted(rows, key=lambda key: (key[:-1], ["sendto", "connected", "sendmmsg", "gso"].index(key[-1]))):
        items = rows[key]
        size, last, burst, sockets, endpoints, copy, mode = key
        cycles = statistics.median(item["cycles_per_packet"] for item in items)
        user = statistics.median(item["user_cycles_per_packet"] for item in items)
        instructions = statistics.median(item["instructions_per_packet"] for item in items)
        calls = sum(item["calls"] for item in items) / max(sum(item["sent"] for item in items), 1)
        ok = all(item["received_bytes"] == item["expected_bytes"] and item["sent"] == item["received"] and not item["reordered"] and
                 not item["corrupt"] and not item["mismatched"] and not item["errors"] for item in items)
        lost = sum(item["sent"] - item["received"] for item in items)
        base = baseline.get(key[:-1])
        relative = f"{(cycles / base - 1) * 100:+.1f}%" if base else "-"
        spread = f" ({min(item['cycles_per_packet'] for item in items):.0f}-{max(item['cycles_per_packet'] for item in items):.0f})"
        print(f"| {size}/{last} | {burst} | {sockets}x{endpoints} | {copy} | {mode} | {len(items)} | {cycles:.0f}{spread} | {user:.0f} | {instructions:.0f} | "
              f"{calls:.3f} | {relative} | {'ok' if ok else f'FAIL lost={lost}'} | {sum(i['again'] for i in items)}/{sum(i['short_sends'] for i in items)} | "
              f"{statistics.median(i['latency_us_p50'] for i in items):.1f}/{statistics.median(i['latency_us_p99'] for i in items):.1f} |")


if __name__ == "__main__":
    main()
