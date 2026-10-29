#!/usr/bin/env python3
"""P0-M 采样器：按固定间隔连续记录 DUT 的内存、句柄、队列与 CPU，直到停止文件出现。

独立进程，逐行 flush，保证即使宿主 SSH 断开也不丢样本。只读，不改变被测系统状态。
"""
import argparse
import os
import pathlib
import time

COLS = [
    "wall", "mono", "pss_kib", "rss_kib", "vm_hwm_kib", "vm_size_kib", "threads",
    "pss_dirty_kib", "lazy_free_kib", "anon_kib",
    "fds", "sockets", "udp_rx_queue_bytes", "udp_tx_queue_bytes", "udp_drops",
    "udp_in_errors", "udp_snd_errors", "udp_rcv_errors", "tcp_retrans", "tcp_retrans_rate",
    "cpu_pct", "cpu_user_pct", "cpu_sys_pct", "proc_user_jiffies", "proc_sys_jiffies", "max_core_pct",
]


def read_status(pid):
    out = {}
    for line in pathlib.Path(f"/proc/{pid}/status").read_text().splitlines():
        key, _, val = line.partition(":")
        out[key] = val.strip().split()[0] if val.strip() else ""
    return out


def read_rollup(pid):
    out = {}
    for line in pathlib.Path(f"/proc/{pid}/smaps_rollup").read_text().splitlines():
        key, _, rest = line.partition(":")
        parts = rest.split()
        if parts:
            out[key] = int(parts[0])
    return out


def count_fds(pid):
    return len(os.listdir(f"/proc/{pid}/fd"))


def count_sockets():
    n = 0
    for path in ("/proc/net/tcp", "/proc/net/tcp6", "/proc/net/udp", "/proc/net/udp6"):
        try:
            n += max(0, len(pathlib.Path(path).read_text().splitlines()) - 1)
        except OSError:
            pass
    return n


def udp_counters():
    """/proc/net/udp 字段：4 是 tx_queue:rx_queue（字节，16 进制），12 是 drops。"""
    rxq = txq = drops = in_err = snd_err = rcv_err = 0
    for path in ("/proc/net/udp", "/proc/net/udp6"):
        try:
            lines = pathlib.Path(path).read_text().splitlines()[1:]
        except OSError:
            continue
        for line in lines:
            f = line.split()
            if len(f) < 13:
                continue
            try:
                tx, rx = f[4].split(":")
                txq += int(tx, 16)
                rxq += int(rx, 16)
                drops += int(f[12])
            except (ValueError, IndexError):
                continue
    try:
        lines = pathlib.Path("/proc/net/snmp").read_text().splitlines()
        idx = next(i for i, line in enumerate(lines) if line.startswith("Udp:"))
        names = lines[idx].split()[1:]
        vals = lines[idx + 1].split()[1:]
        get = lambda k: int(vals[names.index(k)])
        in_err, snd_err, rcv_err = get("InErrors"), get("SndbufErrors"), get("RcvbufErrors")
    except (OSError, ValueError, IndexError, StopIteration):
        pass
    return rxq, txq, drops, in_err, snd_err, rcv_err


def tcp_retrans():
    """RetransSegs 在 /proc/net/snmp 的 Tcp: 段。"""
    try:
        lines = pathlib.Path("/proc/net/snmp").read_text().splitlines()
        idx = next(i for i, l in enumerate(lines) if l.startswith("Tcp:"))
        names = lines[idx].split()[1:]
        vals = lines[idx + 1].split()[1:]
        return int(vals[names.index("RetransSegs")])
    except (OSError, ValueError, IndexError, StopIteration):
        return 0


def cpu_snapshot():
    total = idle = 0
    cores = []
    for line in pathlib.Path("/proc/stat").read_text().splitlines():
        if not line.startswith("cpu"):
            continue
        name = line.split()[0]
        vals = [int(x) for x in line.split()[1:]]
        busy = sum(vals) - vals[3] - vals[4]
        if name == "cpu":
            total, idle = sum(vals), vals[3] + vals[4]
        else:
            cores.append((sum(vals), busy))
    puser = psys = 0
    for task in pathlib.Path(f"/proc/{PID}/task").glob("*/stat"):
        try:
            fields = task.read_text().rsplit(") ", 1)[1].split()
        except OSError:
            continue
        puser += int(fields[11])
        psys += int(fields[12])
    return total, idle, cores, puser, psys


ap = argparse.ArgumentParser()
ap.add_argument("--pid", type=int, required=True)
ap.add_argument("--interval", type=float, default=5.0)
ap.add_argument("--out", required=True)
ap.add_argument("--stop-file", required=True)
args = ap.parse_args()
PID = args.pid

prev_total = prev_idle = None
prev_cores = None
prev_retrans = None
prev_time = None

out = pathlib.Path(args.out).open("w")
out.write(",".join(COLS) + "\n")
stop = pathlib.Path(args.stop_file)

while not stop.exists():
    t0 = time.monotonic()
    if not pathlib.Path(f"/proc/{PID}").exists():
        print(f"target pid {PID} gone, stop sampling", flush=True)
        break
    st = read_status(PID)
    roll = read_rollup(PID)
    rxq, txq, drops, in_err, snd_err, rcv_err = udp_counters()
    total, idle, cores, puser, psys = cpu_snapshot()
    retrans = tcp_retrans()

    cpu = cpu_user = cpu_sys = max_core = 0.0
    if prev_total is not None:
        dt_total = total - prev_total
        dt_idle = idle - prev_idle
        if dt_total > 0:
            cpu = 100.0 * (dt_total - dt_idle) / dt_total
            cpu_sys = 100.0 * sum(max(0, c[1] - p[1]) for c, p in zip(cores, prev_cores)) / dt_total
            cpu_user = cpu - cpu_sys
            max_core = max(100.0 * (c[1] - p[1]) / (c[0] - p[0]) if c[0] > p[0] else 0.0
                           for c, p in zip(cores, prev_cores))
    rate = 0.0
    if prev_retrans is not None and prev_time:
        dsec = t0 - prev_time
        if dsec > 0:
            rate = (retrans - prev_retrans) / dsec

    row = [time.time(), t0, roll.get("Pss", 0), int(st.get("VmRSS", 0)), int(st.get("VmHWM", 0)),
           int(st.get("VmSize", 0)), int(st.get("Threads", 0)),
           roll.get("Pss_Dirty", 0), roll.get("LazyFree", 0), roll.get("Anonymous", 0),
           count_fds(PID), count_sockets(),
           rxq, txq, drops, in_err, snd_err, rcv_err, retrans, round(rate, 3),
           round(cpu, 2), round(cpu_user, 2), round(cpu_sys, 2), puser, psys, round(max_core, 1)]
    out.write(",".join(str(x) for x in row) + "\n")
    out.flush()

    prev_total, prev_idle, prev_cores = total, idle, cores
    prev_retrans, prev_time = retrans, t0
    time.sleep(max(0.0, args.interval - (time.monotonic() - t0)))

out.close()
