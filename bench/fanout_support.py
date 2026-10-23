import os
import signal
import socket
import subprocess
import time
from pathlib import Path


def benchmark_head():
    return os.environ.get("MEDIA_SERVER_BENCH_HEAD") or subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip()


def proc_cpu(pid, tid=None):
    path = Path(f"/proc/{pid}/stat") if tid is None else Path(f"/proc/{pid}/task/{tid}/stat")
    stat = path.read_text().rsplit(") ", 1)[1].split()
    return (int(stat[11]) + int(stat[12])) / os.sysconf("SC_CLK_TCK")


def proc_status(pid, tid=None):
    path = Path(f"/proc/{pid}/status") if tid is None else Path(f"/proc/{pid}/task/{tid}/status")
    values = {}
    for line in path.read_text().splitlines():
        key, _, value = line.partition(":")
        if key in {"VmRSS", "VmSize", "Threads", "voluntary_ctxt_switches", "nonvoluntary_ctxt_switches"}:
            values[key] = int(value.split()[0])
    return values


def proc_identity(pid):
    fields = Path(f"/proc/{pid}/stat").read_text().rsplit(") ", 1)[1].split()
    return {"starttick": int(fields[19]), "exe": str(Path(f"/proc/{pid}/exe").readlink()),
            "netns": str(Path(f"/proc/{pid}/ns/net").readlink())}


def socket_queues(pid):
    queues = {}
    for family in ("tcp", "tcp6", "udp", "udp6"):
        for line in Path(f"/proc/{pid}/net/{family}").read_text().splitlines()[1:]:
            fields = line.split()
            tx, rx = fields[4].split(":")
            queues[fields[9]] = (family[:3], int(tx, 16), int(rx, 16), int(fields[-1]) if family.startswith("udp") else 0)
    return queues


def proc_snapshot(pid, netns_queues=None, sockets=True):
    identity = proc_identity(pid)
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
    descriptors = os.listdir(f"/proc/{pid}/fd")
    queues_by_family = {}
    if sockets:
        inodes = set()
        for descriptor in descriptors:
            try:
                target = os.readlink(f"/proc/{pid}/fd/{descriptor}")
            except FileNotFoundError:
                continue
            if target.startswith("socket:["):
                inodes.add(target[8:-1])
        netns_queues = {} if netns_queues is None else netns_queues
        if identity["netns"] not in netns_queues:
            netns_queues[identity["netns"]] = socket_queues(pid)
        queues = netns_queues[identity["netns"]]
        queues_by_family = {"udp_tx_queue_bytes": 0, "udp_rx_queue_bytes": 0, "udp_drops": 0, "tcp_tx_queue_bytes": 0, "tcp_rx_queue_bytes": 0}
        for socket_inode in inodes:
            family, tx, rx, drops = queues.get(socket_inode, ("", 0, 0, 0))
            if family:
                queues_by_family[f"{family}_tx_queue_bytes"] += tx
                queues_by_family[f"{family}_rx_queue_bytes"] += rx
                queues_by_family["udp_drops"] += drops
    snmp = {}
    for table in ("snmp", "netstat"):
        lines = Path(f"/proc/{pid}/net/{table}").read_text().splitlines()
        for names, values in zip(lines[::2], lines[1::2]):
            prefix = names.split()[0].rstrip(":")
            for key, value in zip(names.split()[1:], values.split()[1:]):
                if key in {"RetransSegs", "InErrors", "RcvbufErrors", "SndbufErrors", "ListenOverflows", "ListenDrops", "TCPLossProbes",
                           "TCPTimeouts", "TCPSpuriousRTOs", "TCPFastRetrans", "TCPBacklogDrop", "TCPRcvQDrop", "TCPZeroWindowDrop", "PruneCalled", "RcvPruned"}:
                    snmp[prefix + key] = int(value)
    status = proc_status(pid)
    if "VmRSS" not in status:
        raise ProcessLookupError(pid)
    if proc_identity(pid) != identity:
        raise RuntimeError(f"process identity changed while sampling PID {pid}")
    return {
        "identity": identity,
        "monotonic_ns": time.monotonic_ns(),
        "cpu_affinity": sorted(os.sched_getaffinity(pid)),
        "cpu": proc_cpu(pid),
        "namespace_snmp": snmp,
        "rss_kib": status["VmRSS"],
        "vmsize_kib": status["VmSize"],
        "thread_count": status["Threads"],
        "pss_kib": pss_kib,
        "fd": len(descriptors),
        "threads": tasks,
        **queues_by_family,
    }


def thread_rates(before, after, field, elapsed):
    return {
        tid: (state[field] - before["threads"].get(tid, {field: state[field]})[field]) / elapsed
        for tid, state in after["threads"].items()
    }


def wait_for_listener(host, port, check=None):
    for _ in range(100):
        if check is not None:
            check()
        try:
            with socket.create_connection((host, port), timeout=0.2):
                return
        except OSError:
            time.sleep(0.1)
    raise RuntimeError("media server did not open RTMP listener")


def wait_for_stream(host, port, name="live/perf0"):
    for _ in range(100):
        try:
            with socket.create_connection((host, port), timeout=2) as connection:
                connection.sendall(f"GET /{name}.flv HTTP/1.1\r\nHost: {host}\r\n\r\n".encode())
                if connection.recv(64).startswith(b"HTTP/1.1 200"):
                    return
        except OSError:
            pass
        time.sleep(0.1)
    raise RuntimeError(f"source {name} did not become readable")


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
