#!/usr/bin/env python3
import re
import sys
from collections import defaultdict
from pathlib import Path

HZ = 100


def threads(path):
    rows = {}
    for line in path.read_text().split("\n"):
        fields = line.split()
        if len(fields) == 4:
            rows[fields[0]] = (int(fields[1]) + int(fields[2]), int(fields[3]))
    return rows


def sockets(path):
    rows = {}
    port = None
    for line in path.read_text().split("\n"):
        match = re.match(r"UNCONN\s+(\d+)\s+\d+\s+198\.19\.0\.10:(\d+)\s", line)
        if match:
            port = int(match.group(2))
            rows[port] = {"recvq": int(match.group(1))}
            continue
        match = re.search(r"skmem:\(r(\d+),rb(\d+).*d(\d+)\)", line)
        if match and port is not None:
            rows[port].update(rb=int(match.group(2)), drops=int(match.group(3)))
            port = None
    return rows


def bt(path):
    packets, maps = defaultdict(dict), defaultdict(dict)
    hist = {}
    for line in path.read_text().split("\n"):
        match = re.match(r"@pk\[(\d+), (\d+)\]: (\d+)", line)
        if match:
            packets[match.group(1)][int(match.group(2))] = int(match.group(3))
            continue
        match = re.match(r"@(\w+)\[(\d+)\]: (\d+)", line)
        if match:
            maps[match.group(1)][match.group(2)] = int(match.group(3))
            continue
        match = re.match(r"@(\w+_max_us): (\d+)", line)
        if match:
            hist[match.group(1)] = int(match.group(2))
    return packets, maps, hist


def perf_threads(path):
    rows = defaultdict(dict)
    if not path.exists():
        return rows
    for line in path.read_text().split("\n"):
        fields = line.split("\t")
        if len(fields) > 3 and fields[1].isdigit():
            rows[fields[0].rsplit("-", 1)[1]][fields[3]] = int(fields[1])
    return rows


for directory in map(Path, sys.argv[1:]):
    counters = perf_threads(directory / "perf-thread.txt")
    seconds = float((directory / "time-b.txt").read_text()) - float((directory / "time-a.txt").read_text())
    before, after = threads(directory / "threads-a.txt"), threads(directory / "threads-b.txt")
    packets, maps, hist = bt(directory / "bt.txt")
    sock_a, sock_c = sockets(directory / "ss-a.txt"), sockets(directory / "ss-c.txt")
    owner = {port: tid for tid, ports in packets.items() for port in ports}
    drop_seconds = float((directory / "time-c.txt").read_text()) - float((directory / "time-a.txt").read_text())
    summary = (directory / "summary.txt").read_text().split()[:3]
    print(f"== {directory.name} {' '.join(summary)} bt_window={seconds:.1f}s transcode_max_us={hist.get('tc_max_us')} srtp_max_us={hist.get('srtp_max_us')}")
    print("tid       cpu  core sessions  pkt/s  transcode_cpu  srtp_cpu  srtp/pkt_us  drops/s  max_recvq  Gcyc/s  Ginst/s")
    worker_tids = sorted(packets, key=lambda tid: -len(packets[tid]))
    for tid in worker_tids:
        cpu = (after[tid][0] - before[tid][0]) / HZ / seconds
        ports = packets[tid]
        drops = sum(sock_c.get(port, {}).get("drops", 0) - sock_a.get(port, {}).get("drops", 0) for port in ports)
        recvq = max(sock_c.get(port, {}).get("recvq", 0) for port in ports)
        rate = sum(ports.values()) / seconds
        tc = maps["tc_sum_us"].get(tid, 0) / 1e6 / seconds
        srtp = maps["srtp_sum_us"].get(tid, 0) / 1e6 / seconds
        per_packet = maps["srtp_sum_us"].get(tid, 0) / max(maps["srtp_n"].get(tid, 1), 1)
        print(f"{tid:8} {cpu:5.3f} {after[tid][1]:4} {len(ports):8} {rate:7.0f} {tc:13.3f} {srtp:9.3f} {per_packet:11.1f} {drops / drop_seconds:8.0f} {recvq:10} {counters[tid].get('cycles', 0) / 8e9:7.2f} {counters[tid].get('instructions', 0) / 8e9:8.2f}")
