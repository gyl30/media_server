import os
import signal
import socket
import subprocess
import time
from pathlib import Path


def proc_cpu(pid, tid=None):
    path = Path(f"/proc/{pid}/stat") if tid is None else Path(f"/proc/{pid}/task/{tid}/stat")
    stat = path.read_text().rsplit(") ", 1)[1].split()
    return (int(stat[11]) + int(stat[12])) / os.sysconf("SC_CLK_TCK")


def proc_status(pid, tid=None):
    path = Path(f"/proc/{pid}/status") if tid is None else Path(f"/proc/{pid}/task/{tid}/status")
    values = {}
    for line in path.read_text().splitlines():
        key, _, value = line.partition(":")
        if key in {"VmRSS", "voluntary_ctxt_switches", "nonvoluntary_ctxt_switches"}:
            values[key] = int(value.split()[0])
    return values


def proc_snapshot(pid):
    tasks = {}
    for task in Path(f"/proc/{pid}/task").iterdir():
        try:
            task_status = proc_status(pid, task.name)
            tasks[task.name] = {
                "cpu": proc_cpu(pid, task.name),
                "context_switches": task_status["voluntary_ctxt_switches"]
                + task_status["nonvoluntary_ctxt_switches"],
                "migrations": next(
                    int(line.split(":", 1)[1])
                    for line in Path(f"/proc/{pid}/task/{task.name}/sched").read_text().splitlines()
                    if line.startswith("se.nr_migrations")
                ),
            }
        except (FileNotFoundError, ProcessLookupError):
            continue

    pss_kib = 0
    for line in Path(f"/proc/{pid}/smaps_rollup").read_text().splitlines():
        if line.startswith("Pss:"):
            pss_kib = int(line.split()[1])
            break
    return {
        "cpu": proc_cpu(pid),
        "rss_kib": proc_status(pid)["VmRSS"],
        "pss_kib": pss_kib,
        "fd": len(list(Path(f"/proc/{pid}/fd").iterdir())),
        "threads": tasks,
    }


def thread_rates(before, after, field, elapsed):
    return {
        tid: (state[field] - before["threads"].get(tid, {field: state[field]})[field]) / elapsed
        for tid, state in after["threads"].items()
    }


def wait_for_listener(host, port):
    for _ in range(100):
        try:
            with socket.create_connection((host, port), timeout=0.2):
                return
        except OSError:
            time.sleep(0.1)
    raise RuntimeError("media server did not open RTMP listener")


def wait_for_stream(host, port):
    for _ in range(100):
        try:
            with socket.create_connection((host, port), timeout=2) as connection:
                connection.sendall(f"GET /live/perf0.flv HTTP/1.1\r\nHost: {host}\r\n\r\n".encode())
                if connection.recv(64).startswith(b"HTTP/1.1 200"):
                    return
        except OSError:
            pass
        time.sleep(0.1)
    raise RuntimeError("source perf0 did not become readable")


def wait_for_phase(path, process, phase, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        for line in path.read_text().splitlines():
            if line.startswith(f"phase={phase} ") or line == f"phase={phase}":
                return line
        if process.poll() is not None:
            raise RuntimeError(f"fanout client exited {process.returncode} before {phase}: {path.read_text()}")
        time.sleep(0.05)
    raise RuntimeError(f"fanout client did not reach {phase} within {timeout}s: {path.read_text()}")


def parse_phase(line):
    values = {}
    for field in line.split():
        key, _, value = field.partition("=")
        if value:
            try:
                values[key] = float(value) if any(char in value for char in ".eE") else int(value)
            except ValueError:
                values[key] = value
    return values


def stop_process(process):
    if process is not None and process.poll() is None:
        process.send_signal(signal.SIGTERM)
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
