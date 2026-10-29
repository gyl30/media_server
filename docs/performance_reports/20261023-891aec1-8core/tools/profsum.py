#!/usr/bin/env python3
import re
import sys
from collections import Counter
from pathlib import Path

SERVER_CPUS = range(8)
PROFILE_SECONDS = 15


def perf_stat(path):
    values = {}
    for line in path.read_text().splitlines():
        fields = line.split("\t")
        if len(fields) > 2 and re.fullmatch(r"[0-9.]+", fields[0]):
            values[fields[2]] = float(fields[0])
    return values


def softirq_rates(directory):
    seconds = float((directory / "softirqs-after.time").read_text()) - float((directory / "softirqs-before.time").read_text())
    rows = []
    for name in ("softirqs-before.txt", "softirqs-after.txt"):
        table = {}
        for line in (directory / name).read_text().splitlines()[1:]:
            key, _, values = line.partition(":")
            table[key.strip()] = [int(value) for value in values.split()]
        rows.append(table)
    return {key: sum(rows[1][key][cpu] - rows[0][key][cpu] for cpu in SERVER_CPUS) / seconds
            for key in ("NET_RX", "NET_TX", "TIMER", "SCHED", "RCU") if key in rows[0]}


def syscalls(path, seconds=10):
    result = []
    for line in path.read_text().splitlines():
        fields = line.split()
        if len(fields) == 3 and fields[1].isdigit():
            result.append((fields[0], int(fields[1]) / seconds, float(fields[2]) / seconds / 1e6))
    return result


def offcpu(path):
    totals = Counter()
    for line in path.read_text().splitlines():
        stack, _, value = line.rpartition(" ")
        if not value.isdigit():
            continue
        frames = stack.split(";")
        kernel = [frame for frame in frames if frame.endswith("_[k]")]
        user = [frame for frame in frames[1:] if not frame.endswith("_[k]") and frame != "-"]
        reason = next((frame for frame in kernel if any(key in frame for key in ("epoll", "futex", "rwsem", "mutex", "nanosleep", "schedule_timeout"))), kernel[-1] if kernel else "?")
        anchor = user[-1] if user else frames[0]
        totals[(reason.replace("_[k]", ""), anchor[:60])] += int(value)
    return totals


def main():
    for item in sys.argv[1:]:
        directory = Path(item)
        print(f"== {directory.name} {(directory / 'summary.txt').read_text().split(' evidence=')[0] if (directory / 'summary.txt').exists() else ''}")
        if (directory / "perf-stat.txt").exists():
            stat = perf_stat(directory / "perf-stat.txt")
            cycles, instructions = stat.get("cycles", 0), stat.get("instructions", 0)
            print(f"  cycles/s={cycles / PROFILE_SECONDS / 1e9:.2f}G instr/s={instructions / PROFILE_SECONDS / 1e9:.2f}G ipc={instructions / cycles if cycles else 0:.3f} "
                  f"cache_miss/s={stat.get('cache-misses', 0) / PROFILE_SECONDS / 1e6:.1f}M miss_ratio={stat.get('cache-misses', 0) / stat.get('cache-references', 1):.3f} "
                  f"ctx/s={stat.get('context-switches', 0) / PROFILE_SECONDS / 1e3:.1f}k migr/s={stat.get('cpu-migrations', 0) / PROFILE_SECONDS:.0f} "
                  f"faults/s={stat.get('page-faults', 0) / PROFILE_SECONDS / 1e3:.1f}k")
        if (directory / "perf-stat-imc.txt").exists():
            imc = perf_stat(directory / "perf-stat-imc.txt")
            print(f"  host_memory_read={imc.get('uncore_imc/cas_count_read/', 0) / PROFILE_SECONDS:.0f}MiB/s "
                  f"write={imc.get('uncore_imc/cas_count_write/', 0) / PROFILE_SECONDS:.0f}MiB/s")
        if (directory / "softirqs-after.txt").exists():
            print("  softirq/s " + " ".join(f"{key}={value / 1e3:.1f}k" for key, value in softirq_rates(directory).items()))
        if (directory / "syscount.txt").exists():
            calls = syscalls(directory / "syscount.txt")
            total = sum(rate for _, rate, _ in calls)
            print(f"  syscalls/s total={total / 1e3:.1f}k " + " ".join(f"{name}={rate / 1e3:.1f}k({busy:.2f}c)" for name, rate, busy in calls[:8]))
        if (directory / "offcpu-folded.txt").exists():
            totals = offcpu(directory / "offcpu-folded.txt")
            print("  offcpu(s over 10s, all threads) " + "; ".join(f"{reason}<{anchor}>={value / 1e6:.2f}" for (reason, anchor), value in totals.most_common(6)))
        if (directory / "report-dso.txt").exists():
            print("  dso " + " ".join(" ".join(line.split()[:2]) for line in (directory / "report-dso.txt").read_text().splitlines()[:6]))


if __name__ == "__main__":
    main()
