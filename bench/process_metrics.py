import os
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
