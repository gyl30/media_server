#!/usr/bin/env python3
"""阶段 6 P0 profiling 执行器。

mode=plain：只跑负载 + 四层 CPU 采样（无 profiler 对照）。
mode=prof ：在测量窗口内的稳定段按相串行启动采样器。分相是必须的：DUT 定向 record 与
            每核 record 并发时会互相抢占 PMU 计数器（实测 19 样本 vs 单独运行 1K 样本），
            perf stat 的 cycles/instructions 也会被压到 4% 使能。

相位（相对 DUT 进程出现的时间）：
  phase1  DUT 定向 perf record（DWARF 调用栈，含内核样本）+ off-CPU bpftrace 覆盖整个采样段
  phase2  服务端 8 核系统级 perf record（覆盖 ksoftirqd 与其他进程代跑的 softirq）
  phase3  perf stat（独占 PMU：IPC、上下文切换、迁移、缺页、syscall 计数）

用法：pi_phase6p.py <plan.jsonl> <out_dir>
"""
import json
import os
import pathlib
import signal
import subprocess
import sys
import time

PLAN = pathlib.Path(sys.argv[1]).resolve()
OUT = pathlib.Path(sys.argv[2]).resolve()
OUT.mkdir(parents=True, exist_ok=True)

HOST, CLIENT = "198.19.0.10", "198.19.0.11"
BUDGET = ["--min-free-memory-mib", "8192", "--min-free-disk-mib", "2048", "--max-server-pss-mib", "16384"]
COMMON = ["--host", HOST, "--client-address", CLIENT,
          "--rtmp-port", "11935", "--rtsp-port", "18554", "--http-port", "18080",
          "--workers", "8", "--server-cpus", "0-7", "--client-cpus", "8-15"]
SRV_CORES = "0-7"
STOP_CORES, STOP_STREAK = 6.0, 3
NCPU = 32
SOFTIRQ_NAMES = ("HI", "TIMER", "NET_TX", "NET_RX", "BLOCK", "IRQ_POLL", "TASKLET", "SCHED", "HRTIMER", "RCU")
SYSCALLS = ("sendmsg", "recvmsg", "sendmmsg", "recvmmsg", "sendto", "recvfrom", "writev", "readv",
            "epoll_wait", "futex", "write", "read")
STAT_EVENTS = ("task-clock,cycles,instructions,context-switches,cpu-migrations,page-faults,"
               "minor-faults,major-faults," + ",".join(f"syscalls:sys_enter_{s}" for s in SYSCALLS))
summary_path = OUT / "summary.jsonl"


def read_stat():
    out = {}
    for line in pathlib.Path("/proc/stat").read_text().splitlines():
        if not line.startswith("cpu"):
            continue
        parts = line.split()
        v = [int(x) for x in parts[1:]]
        pad = v + [0] * (10 - len(v))
        out[parts[0]] = {"usr": pad[0] + pad[1], "sys": pad[2] + pad[5], "softirq": pad[6],
                         "idle": pad[3] + pad[4], "total": sum(v)}
    return out


def read_softirqs():
    rows = {}
    for line in pathlib.Path("/proc/softirqs").read_text().splitlines():
        cols = line.split()
        name = cols[0].rstrip(":") if cols else ""
        if name in SOFTIRQ_NAMES:
            rows[name] = [int(x) for x in cols[1:1 + NCPU]]
    return rows


def find_dut():
    """按可执行文件身份定位 DUT，不用容易误匹配 orchestrator 的命令行模糊匹配。"""
    for pid in filter(str.isdigit, os.listdir("/proc")):
        try:
            exe = os.readlink(f"/proc/{pid}/exe")
        except OSError:
            continue
        if not exe.endswith("/build/media_server"):
            continue
        try:
            cmd = pathlib.Path(f"/proc/{pid}/cmdline").read_bytes().decode(errors="ignore").replace("\x00", " ")
        except OSError:
            continue
        if f"--bind-address {HOST}" in cmd:
            return int(pid), exe, cmd
    return None, None, None


def proc_cpu(pid):
    try:
        st = pathlib.Path(f"/proc/{pid}/stat").read_text().rsplit(") ", 1)[1].split()
    except (OSError, ValueError):
        return None
    hz = os.sysconf("SC_CLK_TCK")
    return ((int(st[11]) + int(st[13])) / hz, (int(st[12]) + int(st[14])) / hz)


def thread_cpu(pid):
    out, hz = {}, os.sysconf("SC_CLK_TCK")
    task = f"/proc/{pid}/task"
    for tid in filter(str.isdigit, os.listdir(task)) if os.path.isdir(task) else []:
        try:
            st = pathlib.Path(f"{task}/{tid}/stat").read_text().rsplit(") ", 1)[1].split()
        except (OSError, ValueError):
            continue
        out[tid] = ((int(st[11]) + int(st[13])) / hz, (int(st[12]) + int(st[14])) / hz)
    return out


def fd_limit(pid):
    try:
        for line in pathlib.Path(f"/proc/{pid}/limits").read_text().splitlines():
            if line.startswith("Max open files"):
                return line.split()[3]
    except (OSError, IndexError):
        pass
    return None


def diff_cpu(lo, hi):
    out = {}
    for core, a in lo.items():
        b = hi.get(core)
        if not b or b["total"] <= a["total"]:
            continue
        dt = b["total"] - a["total"]
        out[core] = {"busy": round(1.0 - (b["idle"] - a["idle"]) / dt, 4),
                     "user": round((b["usr"] - a["usr"]) / dt, 4),
                     "system": round((b["sys"] - a["sys"]) / dt, 4),
                     "softirq": round((b["softirq"] - a["softirq"]) / dt, 4)}
    return out


def nearest(series, t):
    return min(series, key=lambda x: abs(x[0] - t))


OFFCPU = """tracepoint:sched:sched_switch /pid == {tgid}/ {{
  @dut[args->prev_pid] = 1;
  if (args->prev_state == 0) {{ @rq_start[args->prev_pid] = nsecs; }} else {{ @blk_start[args->prev_pid] = nsecs; }}
}}
tracepoint:sched:sched_wakeup /@dut[args->pid]/ {{ @wake[args->pid] = nsecs; }}
tracepoint:sched:sched_switch /@dut[args->next_pid]/ {{
  if (@rq_start[args->next_pid]) {{ @rq_wait_us = hist((nsecs - @rq_start[args->next_pid]) / 1000); delete(@rq_start[args->next_pid]); }}
  if (@wake[args->next_pid]) {{ @wake_to_run_us = hist((nsecs - @wake[args->next_pid]) / 1000); delete(@wake[args->next_pid]); }}
  if (@blk_start[args->next_pid]) {{ @blocked_us = hist((nsecs - @blk_start[args->next_pid]) / 1000); delete(@blk_start[args->next_pid]); }}
}}
interval:s:{secs} {{ exit(); }}
"""


def phases(spec, pid, out):
    """相位定义。at 为相对于采样段起点（profile_start）的偏移，单位秒。

    分相串行是必须的：DUT 定向 record 与每核 record 并发时会互相抢占 PMU 计数器
    （实测 19 样本 vs 单独运行 1K 样本），perf stat 的 cycles/instructions 也会被压到 8% 使能。
    """
    out = str(out)
    p1, p2, p3 = spec.get("phase1_seconds", 30), spec.get("phase2_seconds", 15), spec.get("phase3_seconds", 10)
    at2, at3 = p1 + 5, p1 + 5 + p2 + 5
    return [
        {"name": "phase1_dut_record", "at": 0, "secs": p1,
         "cmd": ["timeout", "-s", "INT", str(p1), "perf", "record", "-F", "99",
                 "-g", "--call-graph", "dwarf,16384", "-p", str(pid), "-o", out + ".dut.perf.data"]},
        {"name": "phase2_core_record", "at": at2, "secs": p2,
         "cmd": ["timeout", "-s", "INT", str(p2), "perf", "record", "-F", "39",
                 "-g", "--call-graph", "dwarf,16384", "-C", SRV_CORES, "-o", out + ".cores.perf.data"]},
        {"name": "phase3_stat", "at": at3, "secs": p3,
         "cmd": ["perf", "stat", "-x,", "-o", out + ".stat.csv", "-p", str(pid),
                 "-e", STAT_EVENTS, "sleep", str(p3)]},
        {"name": "offcpu", "at": 0, "secs": at3 + p3 + 5, "helper": True},
    ]


def run_case(spec):
    runtime = pathlib.Path(spec["runtime"]).resolve()
    stem = OUT / spec["id"]
    profiling = spec.get("mode") == "prof"
    cmd = ["./run.sh", "python3", "bench/perf_run.py", "--output", str(stem),
           "--input", spec.get("input", "rtmp"), "--outputs", spec["outputs"],
           "--sources", str(spec.get("sources", 1)),
           "--warmup", str(spec["warmup"]), "--duration", str(spec["duration"]),
           "--group-size", str(spec["group_size"]), *COMMON, *BUDGET]
    log = open(str(stem) + ".perf_run.log", "w")
    prof_log = open(str(stem) + ".profilers.log", "w") if profiling else None
    start = time.time()
    proc = subprocess.Popen(cmd, cwd=runtime, stdout=log, stderr=subprocess.STDOUT,
                            stdin=subprocess.DEVNULL, start_new_session=True)

    stat_series, dut_series, sq_series, thr_series = [], [], [], []
    plan, running, windows = [], [], {}
    pid, exe, cmdline, limits, t_dut = None, None, None, None, 0.0
    prof_t0 = 0.0
    dut_prev, peak, over, stop_reason, thr_lo = None, 0.0, 0, None, None
    prof_span = [None, None]

    def launch_phases():
        """采样段起点：写 off-CPU 请求并建立相位计划，后续相位由主循环按 at 偏移串行启动。"""
        nonlocal prof_t0
        prof_t0 = time.monotonic()
        plan.extend(phases(spec, pid, stem))
        helper = next(p for p in plan if p.get("helper"))
        (stem.parent / (stem.name + ".offcpu.request")).write_text(
            json.dumps({"tgid": pid, "secs": helper["secs"]}) + "\n")
        pathlib.Path(str(stem) + ".offcpu.bt").write_text(OFFCPU.format(tgid=pid, secs=helper["secs"]))
        windows[helper["name"]] = [time.monotonic_ns(), None]

    def launch_phase(ph):
        nonlocal thr_lo
        windows[ph["name"]] = [time.monotonic_ns(), None]
        if prof_span[0] is None:
            prof_span[0] = windows[ph["name"]][0]
            thr_lo = thread_cpu(pid)
        ph["proc"] = subprocess.Popen(ph["cmd"], stdout=prof_log, stderr=subprocess.STDOUT,
                                      start_new_session=True)
        running.append(ph)

    def next_phase():
        """只有前序相位已退出才启动下一个，避免 PMU 互相抢占。"""
        for ph in plan:
            if ph.get("helper") or "proc" in ph:
                continue
            if time.monotonic() - prof_t0 < ph["at"]:
                return None
            if any(p.get("proc") is not None and p["proc"].poll() is None for p in running):
                return None
            return ph
        return None

    while proc.poll() is None:
        now = time.monotonic_ns()
        stat_series.append((now, read_stat()))
        sq_series.append((now, read_softirqs()))
        if pid is None:
            pid, exe, cmdline = find_dut()
            if pid:
                limits, t_dut = fd_limit(pid), time.monotonic()
        for ph in running:
            if windows[ph["name"]][1] is None and ph["proc"].poll() is not None:
                windows[ph["name"]][1] = time.monotonic_ns()
        d = proc_cpu(pid) if pid else None
        if d:
            dut_series.append((now, d))
            thr_series.append((now, thread_cpu(pid)))
            if dut_prev:
                dt = (now - dut_prev[0]) / 1e9
                if dt > 0:
                    cores = ((d[0] + d[1]) - (dut_prev[1][0] + dut_prev[1][1])) / dt
                    peak = max(peak, cores)
                    if cores >= STOP_CORES and time.monotonic() - start > 30:
                        over += 1
                        if over >= STOP_STREAK:
                            stop_reason = "dut_cpu_stop"
                            os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
                            time.sleep(10)
                            if proc.poll() is None:
                                os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
                            break
                    else:
                        over = 0
                    if profiling and not windows and time.monotonic() - t_dut >= spec.get("profile_start", 50):
                        if cores >= 0.3:
                            launch_phases()
                        else:
                            stop_reason = "profile_start_guard_low_cpu"
                    elif profiling and windows:
                        nxt = next_phase()
                        if nxt is not None:
                            launch_phase(nxt)
            dut_prev = (now, d)
        time.sleep(1.0)

    for ph in running:
        ph["proc"].wait(timeout=120)
        if windows[ph["name"]][1] is None:
            windows[ph["name"]][1] = time.monotonic_ns()
    if prof_span[0]:
        prof_span[1] = max((w[1] for w in windows.values() if w[1]), default=None)
    wall = round(time.time() - start, 1)
    log.close()
    if prof_log:
        prof_log.close()

    rf = stem / "result.json"
    result = json.loads(rf.read_text()) if rf.is_file() else None
    pm = (result or {}).get("process_metrics") or {}
    server = pm.get("server") or {}
    clients = {k: v for k, v in pm.items() if k not in ("server", "runner") and not k.startswith("publisher")}
    num = lambda v: v if isinstance(v, (int, float)) else 0
    groups = (result or {}).get("groups") or [{}]
    meas = groups[0].get("measurement") or {}

    off_meta_path = pathlib.Path(str(stem) + ".offcpu.meta.json")
    if profiling:
        for _ in range(40):
            if off_meta_path.is_file():
                break
            time.sleep(1.0)
    if off_meta_path.is_file():
        meta = json.loads(off_meta_path.read_text())
        if "offcpu" in windows:
            windows["offcpu"] = [meta["start_monotonic_ns"], meta["end_monotonic_ns"]]

    row = {"id": spec["id"], "tier": spec["tier"], "mode": spec.get("mode"), "protocol": spec["protocol"],
           "sources": spec.get("sources", 1), "viewers": spec.get("viewers"), "runtime": str(runtime),
           "wall_seconds": wall, "runner_exit": proc.returncode, "stop_reason": stop_reason,
           "dut_pid": pid, "dut_exe": exe, "dut_cmdline": cmdline, "dut_fd_limit": limits,
           "dut_cpu_peak_live": round(peak, 4),
           "server": {"cpu_cores": server.get("cpu_cores"), "pss_kib_max": server.get("pss_kib_max"),
                      "fd_max": server.get("fd_max"), "affinity": server.get("affinity")},
           "client_cpu_sum": round(sum(num(v.get("cpu_cores")) for v in clients.values()), 4),
           "client_processes": len(clients),
           "client_hottest_thread": round(max([num(t) for v in clients.values()
                                               for t in (v.get("thread_cpu_cores") or {}).values()], default=0), 4),
           "status": (result or {}).get("status"), "boundary": (result or {}).get("capacity_boundary"),
           "media": {k: meas.get(k) for k in ("ready", "progressing", "failed", "runtime_failures",
                                              "viewer_bytes_per_second_min", "viewer_packets_min")},
           "result_path": str(rf) if rf.is_file() else None,
           "phase_windows": {k: {"start": v[0], "end": v[1]} for k, v in windows.items()},
           "profile_span": {"start": prof_span[0], "end": prof_span[1]},
           "offcpu_meta": (json.loads(off_meta_path.read_text()) if off_meta_path.is_file() else None),
           "commands": [p.get("cmd") for p in plan], "artifacts": str(stem) + ".*"}
    row["classification"] = ("TEST_INFRA_FAILURE" if proc.returncode not in (0, None) or result is None
                             else "PASS" if row["boundary"] == "HEALTHY" else "UNHEALTHY")

    win = (result or {}).get("requested_window") or {}
    t0, t1 = win.get("start_monotonic_ns"), win.get("end_monotonic_ns")
    row["requested_window"] = {"start": t0, "end": t1}
    if t0 and t1:
        row["windows_inside"] = {k: bool(t0 <= v[0] and (v[1] or 0) <= t1) for k, v in windows.items() if v[0]}
    if t0 and t1 and len(stat_series) > 2 and dut_series and sq_series:
        lo, hi = nearest(stat_series, t0), nearest(stat_series, t1)
        if abs(lo[0] - t0) < 2e9 and abs(hi[0] - t1) < 2e9:
            row["window_aligned"] = {"span_seconds": round((hi[0] - lo[0]) / 1e9, 3),
                                     "per_core": diff_cpu(lo[1], hi[1])}
        dlo, dhi = nearest(dut_series, t0), nearest(dut_series, t1)
        span = (dhi[0] - dlo[0]) / 1e9
        row["window_aligned"].update({"dut_user_cores": round((dhi[1][0] - dlo[1][0]) / span, 4),
                                      "dut_system_cores": round((dhi[1][1] - dlo[1][1]) / span, 4)})
        slo, shi = nearest(sq_series, t0), nearest(sq_series, t1)
        row["softirq_per_cpu"] = {k: [b - a for a, b in zip(slo[1][k], shi[1][k])] for k in slo[1]}
    if prof_span[0] and prof_span[1] and dut_series:
        plo, phi = nearest(dut_series, prof_span[0]), nearest(dut_series, prof_span[1])
        pspan = (phi[0] - plo[0]) / 1e9
        row["profile_span_cpu"] = {"span_seconds": round(pspan, 3),
                                   "dut_user_cores": round((phi[1][0] - plo[1][0]) / pspan, 4),
                                   "dut_system_cores": round((phi[1][1] - plo[1][1]) / pspan, 4)}
        sq_lo, sq_hi = nearest(sq_series, prof_span[0]), nearest(sq_series, prof_span[1])
        row["softirq_per_cpu_profile"] = {k: [b - a for a, b in zip(sq_lo[1][k], sq_hi[1][k])] for k in sq_lo[1]}
        if thr_lo:
            end_thr = max((s for s in thr_series if s[0] <= prof_span[1]), key=lambda x: x[0], default=None)
            if end_thr:
                row["thread_cpu_profile"] = {t: round(((h[0] + h[1]) - thr_lo[t][0] - thr_lo[t][1]) / pspan, 4)
                                             for t, h in end_thr[1].items() if t in thr_lo}
    with summary_path.open("a") as f:
        f.write(json.dumps(row, ensure_ascii=False) + "\n")
    s = row["server"]
    print(f"[{spec['id']}] class={row['classification']} cpu={s.get('cpu_cores')} peak={row['dut_cpu_peak_live']} "
          f"pssMiB={round((s.get('pss_kib_max') or 0)/1024,1)} inside={row.get('windows_inside')} "
          f"cliCPU={row['client_cpu_sum']} wall={wall}s", flush=True)
    return row


plan_cases = [json.loads(l) for l in PLAN.read_text().splitlines() if l.strip()]
print(f"plan cases: {len(plan_cases)}", flush=True)
for spec in plan_cases:
    try:
        run_case(spec)
    except Exception as exc:                                        # noqa: BLE101
        with summary_path.open("a") as f:
            f.write(json.dumps({"id": spec["id"], "tier": spec.get("tier"), "mode": spec.get("mode"),
                                "classification": "TEST_INFRA_FAILURE", "runner_exception": repr(exc)}) + "\n")
        print(f"[{spec['id']}] EXCEPTION {exc!r}", flush=True)
print("DONE")
