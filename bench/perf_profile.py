#!/usr/bin/env python3
"""Collect separate, attributable profiler evidence from an already running DUT.

Use on the TEST host in the workload's network namespace (or from the host with
an explicit DUT PID). Profiling runs are diagnostic, not comparable to normal
capacity runs. Run `heap-report` after a separately profiled process exits.
"""

import argparse
import hashlib
import json
import math
import os
import re
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path


def starttick(pid):
    fields = Path(f"/proc/{pid}/stat").read_text().rsplit(") ", 1)[1].split()
    return int(fields[19])


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def execute(command, output, timeout):
    started = time.monotonic_ns()
    step = {"command": list(map(str, command)), "started_monotonic_ns": started, "output": output.name}
    executable = shutil.which(str(command[0]))
    if executable:
        step.update(tool_path=executable, tool_sha256=sha256(executable))
    process = None
    try:
        with output.open("w") as stream:
            process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=stream, stderr=subprocess.STDOUT,
                                       start_new_session=True, env={**os.environ, "LC_ALL": "C"})
            step["profiler_pid"] = process.pid
            step["exit_code"] = process.wait(timeout=timeout)
    except (OSError, subprocess.TimeoutExpired) as error:
        step["error"] = f"{type(error).__name__}: {error}"
    finally:
        if process is not None:
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                process.wait(timeout=2)
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        step["ended_monotonic_ns"] = time.monotonic_ns()
        step["elapsed_seconds"] = (step["ended_monotonic_ns"] - started) / 1e9
        if output.is_file():
            step["output_sha256"] = sha256(output)
    return step


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", required=True, choices=("cpu-record", "cpu-stat", "offcpu", "heap-report"))
    parser.add_argument("--output", required=True, type=Path, help="new destination directory")
    parser.add_argument("--pid", type=int, help="DUT PID, required except for heap-report")
    parser.add_argument("--expected-starttick", type=int, help="reject stale or reused PID")
    parser.add_argument("--duration", type=int, default=30)
    parser.add_argument("--binary", type=Path, help="the exact matching ELF for heap-report")
    parser.add_argument("--heap-file", type=Path, help="existing jemalloc heap dump for heap-report")
    parser.add_argument("--perf-bin", default="perf")
    parser.add_argument("--offcpu-bin", default="offcputime-bpfcc")
    parser.add_argument("--jeprof-bin", default="jeprof")
    args = parser.parse_args(argv)
    if args.duration <= 0 or args.pid is not None and args.pid <= 0:
        parser.error("positive duration and PID required")
    if args.mode == "heap-report":
        if not args.binary or not args.heap_file:
            parser.error("heap-report requires --binary and --heap-file")
        if args.pid is not None:
            parser.error("heap-report does not take --pid")
    elif args.pid is None or args.binary or args.heap_file:
        parser.error("live sampling requires --pid and no heap-report arguments")
    args.output.mkdir(parents=True, exist_ok=False, mode=0o700)
    result = {"mode": args.mode, "status": "BLOCKED", "duration_seconds": args.duration,
              "source": "diagnostic sampling (never a capacity baseline)", "steps": [],
              "driver_sha256": sha256(Path(__file__)), "host": os.uname().nodename}
    try:
        if args.mode == "heap-report":
            binary, heap = args.binary.resolve(), args.heap_file.resolve()
            if not binary.is_file() or not heap.is_file() or heap.stat().st_size == 0:
                raise ValueError("ELF and nonempty jemalloc heap dump are required")
            result["binary_sha256"] = sha256(binary)
            result["heap_sha256"] = sha256(heap)
            command = [args.jeprof_bin, "--text", "--show_bytes", str(binary), str(heap)]
            step = execute(command, args.output / "jemalloc.txt", 120)
            result["steps"].append(step)
            if step.get("exit_code") != 0 or (args.output / "jemalloc.txt").stat().st_size == 0:
                raise RuntimeError("jeprof did not generate a nonempty report")
            if not re.search(r"(?m)^Total:\s+[0-9.]+\s+B\b", (args.output / "jemalloc.txt").read_text()):
                raise RuntimeError("jeprof did not report an allocation total in bytes")
        else:
            pid = args.pid
            initial = starttick(pid)
            if args.expected_starttick is not None and initial != args.expected_starttick:
                raise ValueError("DUT PID starttick differs; refusing to sample a reused PID")
            result.update(pid=pid, starttick=initial)
            binary = Path(f"/proc/{pid}/exe").resolve(strict=True)
            result.update(binary=str(binary), binary_sha256=sha256(binary))
            if args.mode == "cpu-record":
                data = args.output / "perf.data"
                command = [args.perf_bin, "record", "-F", "99", "-g", "-p", str(pid), "-o", str(data), "--", "sleep", str(args.duration)]
                step = execute(command, args.output / "perf-record.log", args.duration + 60)
                result["steps"].append(step)
                if step.get("exit_code") != 0 or not data.exists() or data.stat().st_size == 0:
                    raise RuntimeError("perf record failed or produced no perf.data")
                command = [args.perf_bin, "report", "--stdio", "--header", "-i", str(data)]
                step = execute(command, args.output / "perf-report.txt", 120)
                result["steps"].append(step)
                if step.get("exit_code") != 0:
                    raise RuntimeError("perf report failed")
                samples = re.findall(r"(?m)^#\s+Samples:\s+([0-9.,]+)\s*([KMG]?)\b", (args.output / "perf-report.txt").read_text())
                result["samples"] = sum(float(number.replace(",", "")) * {"": 1, "K": 1e3, "M": 1e6, "G": 1e9}[unit]
                                        for number, unit in samples)
                if result["samples"] <= 0:
                    raise RuntimeError("perf record produced no verifiable samples")
                result["perf_data_sha256"] = sha256(data)
            elif args.mode == "cpu-stat":
                command = [args.perf_bin, "stat", "-x", "\t", "--no-big-num", "-p", str(pid), "-e",
                           "task-clock,context-switches,cpu-migrations,page-faults,cycles,instructions",
                           "--", "sleep", str(args.duration)]
                step = execute(command, args.output / "perf-stat.txt", args.duration + 60)
                result["steps"].append(step)
                if step.get("exit_code") != 0:
                    raise RuntimeError("perf stat failed")
                counters = {}
                units = {}
                events = ("task-clock", "context-switches", "cpu-migrations", "page-faults", "cycles", "instructions")
                for line in (args.output / "perf-stat.txt").read_text().splitlines():
                    fields = line.split("\t")
                    if len(fields) >= 3 and fields[2] in events:
                        units[fields[2]] = fields[1]
                        try:
                            counters[fields[2]] = float(fields[0])
                        except ValueError:
                            counters[fields[2]] = None
                result["counters"] = counters
                result["counter_units"] = units
                if any(counters.get(event) is None or not math.isfinite(counters[event]) for event in events) or counters.get("task-clock", 0) <= 0:
                    raise RuntimeError("perf stat did not capture all requested counters; unavailable is not zero")
            else:
                command = [args.offcpu_bin, "-f", "-p", str(pid), str(args.duration)]
                step = execute(command, args.output / "offcpu-folded.txt", args.duration + 90)
                result["steps"].append(step)
                if step.get("exit_code") != 0 or (args.output / "offcpu-folded.txt").stat().st_size == 0:
                    raise RuntimeError("offcputime unavailable or returned no stacks; do not infer zero contention")
                stacks = []
                for line in (args.output / "offcpu-folded.txt").read_text().splitlines():
                    fields = line.rsplit(" ", 1)
                    if len(fields) == 2 and ";" in fields[0] and fields[1].isdigit() and int(fields[1]) > 0:
                        stacks.append(int(fields[1]))
                result.update(folded_stacks=len(stacks), offcpu_microseconds=sum(stacks))
                if not stacks:
                    raise RuntimeError("offcputime returned no positive folded stacks; warnings are not waiting evidence")
            if starttick(pid) != initial:
                raise RuntimeError("DUT exited or PID was reused during profiling")
        result["status"] = "PASS"
    except (OSError, ValueError, RuntimeError, KeyboardInterrupt) as error:
        result["reason"] = str(error) or type(error).__name__
    finally:
        (args.output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({"status": result["status"], "mode": args.mode, "result": str(args.output / "result.json")}))
    return 0 if result["status"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
