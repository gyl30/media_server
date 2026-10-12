#!/usr/bin/env python3
"""Capacity schedule over perf_run; unconfirmed failures never establish a server boundary."""

import argparse
import json
import os
import signal
import subprocess
import sys
from pathlib import Path

from perf_run import OUTPUTS


def split_load(total, protocols):
    base, extra = divmod(total, len(protocols))
    if base == 0:
        raise ValueError("viewer count must cover all selected protocols")
    return ",".join(f"{protocol}:{base + (index < extra)}" for index, protocol in enumerate(protocols))


def search(execute, start, step, granularity, maximum, healthy_repeats=3, failed_repeats=2):
    healthy = 0
    failed = None
    count = start
    while count <= maximum:
        boundary = execute(count)
        if boundary == "HEALTHY":
            healthy = count
            count += step
        elif boundary == "SERVER_UNSTABLE":
            failed = count
            break
        else:
            return {"status": "INCONCLUSIVE", "reason": boundary, "healthy_lower_bound": healthy}
    if failed is None:
        return {"status": "INCONCLUSIVE", "reason": "NO_FAILURE_BOUNDARY", "healthy_lower_bound": healthy}
    while failed - healthy > granularity:
        middle = ((healthy + failed) // (2 * granularity)) * granularity
        middle = max(healthy + granularity, middle)
        boundary = execute(middle)
        if boundary == "HEALTHY":
            healthy = middle
        elif boundary == "SERVER_UNSTABLE":
            failed = middle
        else:
            return {"status": "INCONCLUSIVE", "reason": boundary, "healthy_lower_bound": healthy, "failed_upper_bound": failed}
    if healthy == 0:
        return {"status": "INCONCLUSIVE", "reason": "NO_HEALTHY_BOUNDARY", "failed_upper_bound": failed}
    for count, expected in [(healthy, "HEALTHY")] * healthy_repeats + [(failed, "SERVER_UNSTABLE")] * failed_repeats:
        if execute(count) != expected:
            return {"status": "INCONCLUSIVE", "reason": "BOUNDARY_NOT_REPEATABLE", "healthy_lower_bound": healthy, "failed_upper_bound": failed}
    return {"status": "PASS", "healthy_capacity": healthy, "first_failed": failed,
            "overload_diagnostic_counts": [max(failed, ((healthy * 110 + 9999) // 10000) * 100),
                                           ((healthy * 125 + 9999) // 10000) * 100, ((healthy * 150 + 9999) // 10000) * 100]}


def run_round(command, directory, timeout):
    """Keep an explicit, inconclusive record even if perf_run cannot write result.json."""
    log = directory.parent / f"{directory.name}.runner.log"
    exit_code = None
    failure = None
    with log.open("w") as output:
        try:
            process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=output, stderr=subprocess.STDOUT, start_new_session=True)
        except OSError as error:
            failure = f"cannot start runner: {error}"
        else:
            try:
                exit_code = process.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                failure = f"runner exceeded {timeout} seconds"
                os.killpg(process.pid, signal.SIGTERM)
                try:
                    exit_code = process.wait(timeout=30)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    exit_code = process.wait()
            except KeyboardInterrupt:
                os.killpg(process.pid, signal.SIGTERM)
                try:
                    process.wait(timeout=30)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()
                raise
    result_file = directory / "result.json"
    result = None
    if result_file.exists():
        try:
            result = json.loads(result_file.read_text())
        except (ValueError, OSError) as error:
            failure = f"invalid runner result: {error}"
    elif not failure:
        failure = "runner did not write result.json"
    if not isinstance(result, dict) or "status" not in result:
        failure = failure or "runner result missing status"
    if failure or (exit_code != 0 and result and result.get("status") == "PASS"):
        return {"status": "INCONCLUSIVE", "boundary": "RUNNER_FAILED", "error": failure or "runner exited nonzero despite PASS",
                "exit_code": exit_code, "log": str(log), "result": str(result_file) if result_file.exists() else None}
    return {"status": result["status"], "boundary": result.get("capacity_boundary", "UNCONFIRMED"),
            "exit_code": exit_code, "log": str(log), "result": str(result_file)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--protocols", default="rtmp", help="comma-separated protocols; total viewers split evenly")
    parser.add_argument("--start", type=int, default=500)
    parser.add_argument("--step", type=int, default=500)
    parser.add_argument("--granularity", type=int, default=100)
    parser.add_argument("--maximum", type=int, default=10000)
    parser.add_argument("--healthy-repeats", type=int, default=3, help="independent H verification rounds")
    parser.add_argument("--failed-repeats", type=int, default=2, help="independent F verification rounds")
    parser.add_argument("--round-timeout", type=int, default=900, help="seconds before terminating an unresponsive workload")
    parser.add_argument("--counts", help="explicit comma-separated fixed/overload counts, skips search")
    parser.add_argument("--plan-only", action="store_true", help="emit schedule, do not launch a workload")
    parser.add_argument("runner_args", nargs=argparse.REMAINDER, help="after --: perf_run arguments excluding output and outputs")
    args = parser.parse_args()
    protocols = args.protocols.split(",")
    if len(set(protocols)) != len(protocols) or any(protocol not in OUTPUTS for protocol in protocols):
        parser.error("distinct supported output protocols required")
    if min(args.start, args.step, args.granularity, args.maximum, args.healthy_repeats, args.failed_repeats, args.round_timeout) <= 0 or args.start > args.maximum or args.step % args.granularity or args.start % args.granularity:
        parser.error("positive schedule aligned with granularity required")
    counts = [int(value) for value in args.counts.split(",")] if args.counts else list(range(args.start, args.maximum + 1, args.step))
    if any(count < len(protocols) for count in counts):
        parser.error("each count must cover all protocols")
    args.output.mkdir(parents=True, exist_ok=False, mode=0o700)
    rounds = []
    forwarded = args.runner_args[1:] if args.runner_args[:1] == ["--"] else args.runner_args
    if any(value.split("=", 1)[0] in ("--outputs", "--output", "--fault") for value in forwarded):
        parser.error("driver owns output/counts; deliberate faults are not capacity tests")

    def execute(count):
        directory = args.output / f"round-{len(rounds):03d}-{count}"
        command = [sys.executable, str(Path(__file__).with_name("perf_run.py")), "--output", str(directory),
                   "--outputs", split_load(count, protocols), *forwarded]
        round_result = run_round(command, directory, args.round_timeout)
        rounds.append({"viewers": count, **round_result})
        return round_result["boundary"]

    if args.plan_only:
        result = {"status": "NOT_RUN", "counts": [{"viewers": count, "outputs": split_load(count, protocols)} for count in counts],
                  "step": args.step, "granularity": args.granularity}
    elif args.counts:
        for count in counts:
            execute(count)
        result = {"status": "PASS" if all(item["status"] == "PASS" for item in rounds) else "INCONCLUSIVE", "rounds": rounds}
    else:
        result = search(execute, args.start, args.step, args.granularity, args.maximum, args.healthy_repeats, args.failed_repeats)
        result["rounds"] = rounds
    (args.output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result))
    return 0 if result["status"] in ("PASS", "NOT_RUN") else 1


if __name__ == "__main__":
    raise SystemExit(main())
