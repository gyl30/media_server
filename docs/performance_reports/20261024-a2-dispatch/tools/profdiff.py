#!/usr/bin/env python3
import re
import sys
from pathlib import Path

SYSCALL = re.compile(r"^(\w+)\s+(\d+)\s+([\d.]+)$")
NSTAT = ("TcpInSegs", "TcpOutSegs", "TcpExtTCPOrigDataSent", "TcpExtDelayedACKs", "TcpExtTCPHPAcks", "TcpExtTCPPureAcks",
         "TcpExtTCPDelivered", "UdpOutDatagrams", "IpOutRequests", "IpInReceives")


def load(path):
    path = Path(path)
    viewers = int(re.search(r"(\d+)-[AB]", path.name).group(1))
    stat = {}
    for line in (path / "perf-stat.txt").read_text().splitlines():
        fields = line.split("\t")
        if len(fields) > 2 and fields[0].isdigit():
            stat[fields[2]] = int(fields[0])
    seconds = 15.0
    symbols = {}
    for line in (path / "report-sym.txt").read_text().splitlines():
        match = re.match(r"\s*([\d.]+)%\s+(\S+)\s+\[[.k]\]\s+(.+)$", line)
        if match:
            symbols[match.group(3).strip()[:70]] = float(match.group(1)) / 100 * stat["cycles"] / seconds / viewers / 1e6
    before = {}
    after = {}
    for name, target in (("nstat-before.txt", before), ("nstat-after.txt", after)):
        file = path / name
        if file.exists():
            for line in file.read_text().splitlines():
                fields = line.split()
                if len(fields) >= 2 and fields[1].isdigit():
                    target[fields[0]] = int(fields[1])
    elapsed = None
    if (path / "nstat-before.time").exists():
        elapsed = float((path / "nstat-after.time").read_text()) - float((path / "nstat-before.time").read_text())
    nstat = {key: (after[key] - before[key]) / elapsed / viewers for key in NSTAT if key in after and elapsed}
    syscalls = {}
    for line in (path / "syscount.txt").read_text().splitlines():
        match = SYSCALL.match(line.strip())
        if match:
            syscalls[match.group(1)] = int(match.group(2)) / 10 / viewers
    return {"viewers": viewers, "mcyc": stat["cycles"] / seconds / viewers / 1e6, "minstr": stat["instructions"] / seconds / viewers / 1e6,
            "ctx": stat.get("context-switches", 0) / seconds, "symbols": symbols, "nstat": nstat, "syscalls": syscalls}


def main():
    a = load(sys.argv[1])
    b = load(sys.argv[2])
    print(f"{Path(sys.argv[1]).name} vs {Path(sys.argv[2]).name}")
    print(f"Mcycles/viewer {a['mcyc']:.3f} -> {b['mcyc']:.3f} ({(b['mcyc'] / a['mcyc'] - 1) * 100:+.1f}%)  "
          f"Minstr/viewer {a['minstr']:.3f} -> {b['minstr']:.3f} ({(b['minstr'] / a['minstr'] - 1) * 100:+.1f}%)  ctx/s {a['ctx']:.0f} -> {b['ctx']:.0f}")
    print("per viewer per second:")
    for key in NSTAT:
        if key in a["nstat"] and key in b["nstat"]:
            print(f"  {key:24s} {a['nstat'][key]:9.2f} -> {b['nstat'][key]:9.2f}")
    for key in sorted(set(a["syscalls"]) | set(b["syscalls"]), key=lambda name: -max(a["syscalls"].get(name, 0), b["syscalls"].get(name, 0)))[:8]:
        print(f"  syscall {key:16s} {a['syscalls'].get(key, 0):9.3f} -> {b['syscalls'].get(key, 0):9.3f}")
    names = set(a["symbols"]) | set(b["symbols"])
    diff = sorted(names, key=lambda name: b["symbols"].get(name, 0) - a["symbols"].get(name, 0))
    count = int(sys.argv[3]) if len(sys.argv) > 3 else 12
    print("symbols (Mcycles/viewer) largest increase B-A:")
    for name in reversed(diff[-count:]):
        print(f"  {b['symbols'].get(name, 0) - a['symbols'].get(name, 0):+.4f}  {a['symbols'].get(name, 0):.4f} -> {b['symbols'].get(name, 0):.4f}  {name}")
    print("largest decrease:")
    for name in diff[:count // 2]:
        print(f"  {b['symbols'].get(name, 0) - a['symbols'].get(name, 0):+.4f}  {a['symbols'].get(name, 0):.4f} -> {b['symbols'].get(name, 0):.4f}  {name}")


if __name__ == "__main__":
    main()
