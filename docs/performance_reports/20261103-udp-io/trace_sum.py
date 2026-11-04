#!/usr/bin/env python3
"""Summarize udp_trace.bt output (10 s) with the matching perf stat: syscalls per second and their mean time, UDP datagrams sent per
second and their size distribution, and the share of sent datagrams that sit in same-fd runs of each length. Usage:
trace_sum.py trace.txt [stat.txt]"""

import re
import sys

NAMES = {0: "read", 1: "write", 20: "writev", 44: "sendto", 45: "recvfrom", 46: "sendmsg", 47: "recvmsg", 202: "futex",
         232: "epoll_wait", 281: "epoll_pwait", 307: "sendmmsg", 299: "recvmmsg", 28: "madvise", 230: "clock_nanosleep"}
SECONDS = 10.0


def histogram(text, name):
    block = re.search(rf"^@{name}:[ \t]*\n(.*?)(?:\n\s*\n|\Z)", text, re.S | re.M)
    rows = []
    if not block:
        return rows
    for line in block.group(1).splitlines():
        match = re.match(r"^\[(\d+)[,\s]*(\d*)\)?\]?\s+(\d+)", line.strip())
        if match:
            rows.append((int(match.group(1)), int(match.group(3))))
    return rows


def main():
    text = open(sys.argv[1]).read()
    calls = {int(key): int(value) for key, value in re.findall(r"^@calls\[(\d+)\]: (\d+)", text, re.M)}
    times = {int(key): int(value) for key, value in re.findall(r"^@ns\[(\d+)\]: (\d+)", text, re.M)}
    cycles = None
    if len(sys.argv) > 2:
        match = re.search(r"([\d,]+)\s+cycles", open(sys.argv[2]).read())
        cycles = int(match.group(1).replace(",", "")) / 10.0 if match else None
    print("syscalls (per s, mean us, cores):")
    for key, value in sorted(calls.items(), key=lambda item: -times.get(item[0], 0))[:10]:
        print(f"  {str(NAMES.get(key, key)):12s} {value / SECONDS / 1000:8.1f} k/s  {times.get(key, 0) / max(value, 1) / 1000:6.2f} us  {times.get(key, 0) / SECONDS / 1e9:5.2f}")
    tx = histogram(text, "udp_tx_len")
    sent = sum(count for _, count in tx)
    if sent:
        mean = sum((low + 50) * count for low, count in tx) / sent
        print(f"udp datagrams sent {sent / SECONDS / 1000:.1f} k/s, mean ~{mean:.0f} B; by size bucket: " +
              ", ".join(f"{low}-{low + 99}:{count * 100 / sent:.1f}%" for low, count in tx if count * 100 / sent >= 0.5))
        if cycles:
            print(f"server cycles per sent datagram (all work, perf stat window): {cycles / (sent / SECONDS):.0f}")
    rx = histogram(text, "udp_rx_len")
    received = sum(count for _, count in rx)
    if received:
        print(f"udp datagrams received {received / SECONDS / 1000:.1f} k/s; by size bucket: " +
              ", ".join(f"{low}-{low + 99}:{count * 100 / received:.1f}%" for low, count in rx if count * 100 / received >= 0.5))
    burst = histogram(text, "burst")
    packets = sum(length * count for length, count in burst)
    if packets:
        share = {length: length * count * 100 / packets for length, count in burst}
        print("same-fd send runs, share of send calls by run length: " + ", ".join(f"{length}:{value:.1f}%" for length, value in share.items() if value >= 0.5))
        print(f"  mean run length {packets / sum(count for _, count in burst):.2f}; calls in runs >=2: {sum(v for k, v in share.items() if k >= 2):.1f}%, >=4: {sum(v for k, v in share.items() if k >= 4):.1f}%")


if __name__ == "__main__":
    main()
