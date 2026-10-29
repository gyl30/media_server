#!/usr/bin/env python3
"""阶段 6 配对标定执行器：在两个运行包之间交替执行同一档位，并采集四层 CPU 口径。

四层口径：
1. DUT 进程 CPU —— 由 perf_run 在测量窗口首末取值（权威口径，本脚本不改它）；
2. DUT 进程 user/system —— /proc/<pid>/stat 的 utime/stime 1 Hz 时间序列，分析时按
   result.json 的 requested_window（CLOCK_MONOTONIC）端点对齐，与第 1 层同区间；
3. per-core busy/user/system/softirq —— /proc/stat 每 CPU 字段，同窗口对齐；
4. softirq 类型归因 —— /proc/softirqs 窗口前后差值按 CPU × 类型。

用法：pi_phase6.py <plan.jsonl> <out_dir>
计划每行需含 id、baseline(A/B)、runtime(运行包根目录) 与负载参数。
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

HOST = "198.19.0.10"
CLIENT = "198.19.0.11"
BUDGET = ["--min-free-memory-mib", "8192", "--min-free-disk-mib", "2048", "--max-server-pss-mib", "16384"]
COMMON = ["--host", HOST, "--client-address", CLIENT,
          "--rtmp-port", "11935", "--rtsp-port", "18554", "--http-port", "18080",
          "--workers", "8", "--server-cpus", "0-7", "--client-cpus", "8-15"]
STOP_CORES = 6.0
STOP_STREAK = 3
NCPU = 32
SOFTIRQ_NAMES = ("HI", "TIMER", "NET_TX", "NET_RX", "BLOCK", "IRQ_POLL", "TASKLET", "SCHED", "HRTIMER", "RCU")
summary_path = OUT / "summary.jsonl"


def read_stat():
    """每 CPU 累计时间：user/nice、system+irq、softirq、idle+iowait、total。"""
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
        if not cols or cols[0].rstrip(":") not in SOFTIRQ_NAMES:
            continue
        rows[cols[0].rstrip(":")] = [int(x) for x in cols[1:1 + NCPU]]
    return rows


def find_dut():
    for pid in filter(str.isdigit, os.listdir("/proc")):
        try:
            cmd = pathlib.Path(f"/proc/{pid}/cmdline").read_bytes().decode(errors="ignore")
        except OSError:
            continue
        if "media_server" in cmd and f"--bind-address {HOST}" in cmd.replace("\x00", " "):
            return pid
    return None


def dut_cpu(pid):
    """(utime, stime) 秒。fields: ... utime(14) stime(15) cutime(16) cstime(17)。"""
    try:
        st = pathlib.Path(f"/proc/{pid}/stat").read_text().rsplit(") ", 1)[1].split()
    except (OSError, ValueError):
        return None
    hz = os.sysconf("SC_CLK_TCK")
    return ((int(st[11]) + int(st[13])) / hz, (int(st[12]) + int(st[14])) / hz)


def fd_limit(pid):
    try:
        for line in pathlib.Path(f"/proc/{pid}/limits").read_text().splitlines():
            if line.startswith("Max open files"):
                return line.split()[3]
    except (OSError, IndexError):
        pass
    return None


def align(series, t0, t1):
    """取最接近窗口端点的两个采样并求差；series 元素为 (monotonic_ns, payload)。"""
    lo = min(series, key=lambda x: abs(x[0] - t0))
    hi = min(series, key=lambda x: abs(x[0] - t1))
    if hi[0] <= lo[0] or abs(lo[0] - t0) > 2e9 or abs(hi[0] - t1) > 2e9:
        return None
    return lo, hi


def diff_cpu(lo, hi, span):
    """lo/hi 为两个采样点的每 CPU 累计字典；span 未使用，保留以便调用处对齐语义。"""
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


def classify(row, result):
    if row.get("runner_exception") or result is None or row.get("runner_exit") not in (0, None):
        return "TEST_INFRA_FAILURE"
    boundary = result.get("capacity_boundary")
    if row.get("stop_reason") == "dut_cpu_stop":
        return "BUDGET_STOP"
    if boundary == "CLIENT_RESOURCE":
        return "CLIENT_RESOURCE"
    if boundary == "UPSTREAM_FAILED":
        return "UPSTREAM_RESOURCE"
    if boundary == "SERVER_UNSTABLE":
        return "DUT_FAILURE"
    if result.get("status") == "PASS" and boundary == "HEALTHY":
        return "PASS"
    return "INCONCLUSIVE"


def run_case(spec):
    runtime = pathlib.Path(spec["runtime"]).resolve()
    out = OUT / spec["id"]
    cmd = ["./run.sh", "python3", "bench/perf_run.py", "--output", str(out),
           "--input", spec.get("input", "rtmp"), "--outputs", spec["outputs"],
           "--sources", str(spec.get("sources", 1)),
           "--warmup", str(spec["warmup"]), "--duration", str(spec["duration"]),
           "--group-size", str(spec["group_size"]), *COMMON,
           *(["--client-dir", spec["client_dir"]] if spec.get("client_dir") else []), *BUDGET]
    log = (OUT / (spec["id"] + ".log")).open("w")
    start = time.time()
    proc = subprocess.Popen(cmd, cwd=runtime, stdout=log, stderr=subprocess.STDOUT,
                            stdin=subprocess.DEVNULL, start_new_session=True)
    guard_after = 25 + spec.get("viewers", 0) / 50 + spec["warmup"]
    stat_series, dut_series, sq_series = [], [], []
    dut = None
    limits = None
    over = 0
    peak = 0.0
    dut_prev = None
    stop_reason = None
    while proc.poll() is None:
        now = time.monotonic_ns()
        stat_series.append((now, read_stat()))
        sq_series.append((now, read_softirqs()))
        if dut is None:
            dut = find_dut()
            if dut is not None:
                limits = fd_limit(dut)
        d = dut_cpu(dut) if dut else None
        if d:
            dut_series.append((now, d))
            if dut_prev:
                dt = (now - dut_prev[0]) / 1e9
                if dt > 0:
                    cores = ((d[0] + d[1]) - (dut_prev[1][0] + dut_prev[1][1])) / dt
                    peak = max(peak, cores)
                    if cores >= STOP_CORES and time.monotonic() - start > guard_after:
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
            dut_prev = (now, d)
        time.sleep(1.0)
    wall = round(time.time() - start, 1)
    log.close()

    rf = out / "result.json"
    result = json.loads(rf.read_text()) if rf.is_file() else None
    pm = (result or {}).get("process_metrics") or {}
    server = pm.get("server") or {}
    clients = {k: v for k, v in pm.items() if k not in ("server", "runner") and not k.startswith("publisher")}
    num = lambda v: v if isinstance(v, (int, float)) else 0

    row = {"id": spec["id"], "baseline": spec["baseline"], "tier": spec["tier"],
           "protocol": spec["protocol"], "sources": spec.get("sources", 1), "viewers": spec.get("viewers"),
           "pair_slot": spec.get("pair_slot"), "runtime": str(runtime), "wall_seconds": wall,
           "runner_exit": proc.returncode, "stop_reason": stop_reason, "dut_fd_limit": limits if dut else None,
           "dut_cpu_peak_live": round(peak, 4),
           "server": {"cpu_cores": server.get("cpu_cores"), "pss_kib_max": server.get("pss_kib_max"),
                      "fd_max": server.get("fd_max"), "affinity": server.get("affinity")},
           "client_cpu_sum": round(sum(num(v.get("cpu_cores")) for v in clients.values()), 4),
           "client_hottest_thread": round(max([num(t) for v in clients.values()
                                               for t in (v.get("thread_cpu_cores") or {}).values()], default=0), 4),
           "client_processes": len(clients),
           "status": (result or {}).get("status"), "boundary": (result or {}).get("capacity_boundary"),
           "server_evidence": (result or {}).get("server_evidence"),
           "result_path": str(rf) if rf.is_file() else None}
    row["budget_events"] = [k for k, v in ((row.get("server_evidence") or {}).items())
                            if isinstance(v, (int, float)) and v > 0]
    row["classification"] = classify(row, result)

    win = (result or {}).get("requested_window") or {}
    t0, t1 = win.get("start_monotonic_ns"), win.get("end_monotonic_ns")
    row["window_aligned"] = {}
    if t0 and t1 and len(stat_series) > 2:
        pair = align(stat_series, t0, t1)
        if pair:
            span = (pair[1][0] - pair[0][0]) / 1e9
            row["window_aligned"] = {"span_seconds": round(span, 3), "per_core": diff_cpu(pair[0][1], pair[1][1], span)}
        dpair = align(dut_series, t0, t1)
        if dpair:
            span = (dpair[1][0] - dpair[0][0]) / 1e9
            row["window_aligned"].update({
                "dut_user_cores": round((dpair[1][1][0] - dpair[0][1][0]) / span, 4),
                "dut_system_cores": round((dpair[1][1][1] - dpair[0][1][1]) / span, 4)})
        if sq_series and len(sq_series) > 2:
            spair = align(sq_series, t0, t1)
            if spair:
                row["softirq_per_cpu"] = {name: [b - a for a, b in zip(spair[0][1][name], spair[1][1][name])]
                                          for name in SOFTIRQ_NAMES if name in spair[0][1] and name in spair[1][1]}
    with summary_path.open("a") as f:
        f.write(json.dumps(row, ensure_ascii=False) + "\n")
    s = row["server"]
    wa = row.get("window_aligned") or {}
    print(f"[{spec['id']}] class={row['classification']} cpu={s.get('cpu_cores')} "
          f"user={wa.get('dut_user_cores')} sys={wa.get('dut_system_cores')} peak={row['dut_cpu_peak_live']} "
          f"pssMiB={round((s.get('pss_kib_max') or 0)/1024,1)} cliCPU={row['client_cpu_sum']} wall={wall}s", flush=True)
    return row


plan = [json.loads(l) for l in PLAN.read_text().splitlines() if l.strip()]
print(f"plan cases: {len(plan)}", flush=True)
for spec in plan:
    try:
        run_case(spec)
    except Exception as exc:                                        # noqa: BLE101
        with summary_path.open("a") as f:
            f.write(json.dumps({"id": spec["id"], "baseline": spec.get("baseline"), "tier": spec.get("tier"),
                                "classification": "TEST_INFRA_FAILURE", "runner_exception": repr(exc)}) + "\n")
        print(f"[{spec['id']}] EXCEPTION {exc!r}", flush=True)
print("DONE")
