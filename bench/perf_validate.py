#!/usr/bin/env python3
"""Small destructive NEGATIVE tests for the existing perf_run harness, never capacity data."""

import argparse
import json
import os
import signal
import subprocess
import sys
import time
from pathlib import Path

CASES = ("invalid_token", "expired_token", "replayed_token", "source_stop", "cancel")


def verdict(case, result, was_cancelled):
    if not isinstance(result, dict):
        return "FAIL", "runner did not produce a result object"
    cleanup = result.get("cleanup", {})
    if cleanup.get("all_owned_processes_reaped") is not True or cleanup.get("remaining_owned_receivers") or cleanup.get("errors"):
        return "FAIL", "owned processes or receivers were not cleaned up"
    if result.get("status") == "PASS":
        return "FAIL", "negative fault incorrectly accepted as healthy"
    if result.get("status") != "FAIL":
        return "INCONCLUSIVE", "runner did not complete a confirmed negative failure"
    if result.get("capacity_boundary") in ("SERVER_UNSTABLE", "CLIENT_RESOURCE"):
        return "INCONCLUSIVE", "resource/server boundary conflicts with targeted negative test"
    groups = result.get("groups", [])
    if case == "cancel":
        if was_cancelled and any(error.get("type") == "KeyboardInterrupt" for error in result.get("errors", [])):
            return "PASS", "runner interruption and cleanup observed"
        return "FAIL", "no confirmed mid-measurement interruption"
    if not groups:
        return "FAIL", "no viewer group evidence"
    if case == "source_stop":
        fault = result.get("fault_evidence", {})
        window = result.get("requested_window", {})
        if not (fault.get("kind") == "source_stop" and fault.get("media_ready_groups") == len(groups) and
                window.get("start_monotonic_ns", 0) < fault.get("requested_monotonic_ns", 0) <= fault.get("completed_monotonic_ns", 0) < window.get("end_monotonic_ns", 0) and
                fault.get("publishers") and all(item.get("exit_code") is not None for item in fault["publishers"])):
            return "INCONCLUSIVE", "source stop was not observed after all viewers became media-ready"
        if all(group.get("failure_category") in ("SOURCE_ENDED", "MEDIA_STALL") and
               group.get("measurement", {}).get("ready") == group.get("requested") and
               group.get("measurement", {}).get("measurement_start_monotonic_ns", 0) > 0 and
               group.get("measurement", {}).get("failed", 0) > 0 for group in groups):
            return "PASS", "media interruption detected"
        return "INCONCLUSIVE", "source stop did not produce a confirmed media-progress failure"
    if all(group.get("failure_category") == "AUTHORIZATION" and group.get("measurement", {}).get("ready") == 0 and any(
            indicator in str(error).lower() for error in group.get("first_errors", [])
            for indicator in ("403", "401", "unauthor", "forbidden", "denied", "reject")) for group in groups):
        return "PASS", "client observed authorization rejection"
    return "INCONCLUSIVE", "authorization classification lacks observable rejection evidence"


def stop_group(process):
    if process.poll() is not None:
        return
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        return
    try:
        process.wait(timeout=30)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        process.wait()


def execute(case, directory, forwarded, timeout):
    directory.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    command = [sys.executable, str(Path(__file__).with_name("perf_run.py")),
               "--output", str(directory), "--input", "rtmp", "--outputs", "flv:2", "--sources", "1",
               "--warmup", "1", "--duration", "60" if case == "cancel" else "18",
               "--progress-interval", "2", "--group-size", "1", "--fault", "none" if case == "cancel" else case,
               *forwarded]
    stdout = directory.parent / f"{case}.runner.log"
    interrupted = False
    reason = None
    with stdout.open("w") as log:
        process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            if case == "cancel":
                barrier = directory / "measurement.start"
                deadline = time.monotonic() + timeout
                while not barrier.exists():
                    if process.poll() is not None:
                        reason = "runner exited before measurement barrier"
                        break
                    if time.monotonic() >= deadline:
                        reason = "measurement barrier timed out"
                        break
                    time.sleep(0.05)
                if reason is None:
                    start_ns = int(barrier.read_text())
                    delay = (start_ns - time.monotonic_ns()) / 1e9 + 1
                    if delay > 0:
                        time.sleep(min(delay, timeout))
                    if process.poll() is not None:
                        reason = "runner exited before SIGTERM"
                    else:
                        interrupted = True
                        os.killpg(process.pid, signal.SIGTERM)
            process.wait(timeout=timeout if case != "cancel" else 40)
        except (subprocess.TimeoutExpired, ValueError) as error:
            reason = f"{type(error).__name__}: {error}"
        except KeyboardInterrupt:
            stop_group(process)
            raise
        finally:
            stop_group(process)
    result_file = directory / "result.json"
    try:
        result = json.loads(result_file.read_text())
    except (FileNotFoundError, ValueError):
        result = None
    state, detail = verdict(case, result, interrupted)
    if process.returncode == 0:
        state, detail = "FAIL", "negative runner unexpectedly exited successfully"
    if reason:
        state, detail = "FAIL", reason
    return {"case": case, "status": state, "reason": detail, "runner_exit_code": process.returncode,
            "result": str(result_file) if result_file.exists() else None, "log": str(stdout)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cases", default=",".join(CASES), help="comma-separated negative tests")
    parser.add_argument("--case-timeout", type=int, default=180)
    parser.add_argument("runner_args", nargs=argparse.REMAINDER, help="perf_run arguments after -- (without runner-owned fields)")
    args = parser.parse_args()
    cases = args.cases.split(",")
    if len(set(cases)) != len(cases) or any(case not in CASES for case in cases) or args.case_timeout <= 0:
        parser.error("choose distinct known cases and a positive timeout")
    forwarded = args.runner_args[1:] if args.runner_args[:1] == ["--"] else args.runner_args
    owned = {"--output", "--input", "--outputs", "--sources", "--fault", "--duration", "--warmup", "--progress-interval", "--group-size"}
    if any(value.split("=", 1)[0] in owned for value in forwarded):
        parser.error("runner_args must not override negative-test scenario settings")
    args.output.mkdir(parents=True, exist_ok=False, mode=0o700)
    results = []
    try:
        for case in cases:
            results.append(execute(case, args.output / case, forwarded, args.case_timeout))
    except KeyboardInterrupt:
        results.append({"case": "interrupted", "status": "INCONCLUSIVE", "reason": "operator cancelled validation"})
    summary = {"status": "PASS" if len(results) == len(cases) and all(item["status"] == "PASS" for item in results) else "INCONCLUSIVE",
               "cases": results, "note": "negative scenarios only; never use as server capacity evidence"}
    (args.output / "result.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary))
    return 0 if summary["status"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
