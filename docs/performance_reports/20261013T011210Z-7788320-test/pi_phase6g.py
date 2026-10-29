#!/usr/bin/env python3
"""协议间 CPU 归因：比较两个已完成的画像用例。

对每个用例给出权威 CPU/观看、完整 DSO 归属（0.001% 阈值）、self 热点、perf stat 的
syscall 与 IPC、线程 CPU 分解、softirq 分解，并把百分比统一换算成 mCPU/观看后相减，
避免只比较百分比。

用法：pi_phase6g.py <summary.jsonl> <idA> <idB>
"""
import json
import os
import subprocess
import sys

SUMMARY, ID_A, ID_B = sys.argv[1], sys.argv[2], sys.argv[3]
DSO_LIMIT = "0.001"
SELF_LIMIT = "0.05"
KERNEL_DSOS = ("[kernel.kallsyms]", "[vmlinux]", "[vdso]", "[kallsyms")


def run(cmd, timeout=900):
    return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout).stdout


def load(summary, case_id):
    for line in open(summary):
        d = json.loads(line)
        if d.get("id") == case_id:
            return d
    raise SystemExit(f"用例不存在: {case_id}")


def artifact(cmds, needle):
    for cmd in cmds or []:
        if not cmd or "-o" not in cmd:
            continue
        path = cmd[cmd.index("-o") + 1]
        if needle in path:
            return path
    return None


def dso_attribution(data):
    """perf report 列序：Overhead Command SharedObject [k/.] Symbol，DSO 在 parts[2]。"""
    out = run(["perf", "report", "-i", data, "--stdio", "--no-children", "-g", "none",
               "--percent-limit", DSO_LIMIT])
    agg = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 5 and parts[0].endswith("%") and parts[3] in ("[k]", ".", "[.]", "u"):
            try:
                pct = float(parts[0].rstrip("%"))
            except ValueError:
                continue
            agg[parts[2]] = agg.get(parts[2], 0.0) + pct
    return agg


def self_symbols(data):
    out = run(["perf", "report", "-i", data, "--stdio", "--no-children", "-g", "none",
               "--percent-limit", SELF_LIMIT])
    rows = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 5 and parts[0].endswith("%") and parts[3] in ("[k]", ".", "[.]", "u"):
            try:
                pct = float(parts[0].rstrip("%"))
            except ValueError:
                continue
            rows[(" ".join(parts[4:]), parts[2], parts[3])] = pct
    return rows


def stat_window(vals, fallback):
    """stat 只在 phase3 的短窗内运行。task-clock 是 CPU 时间，需除以 CPUs utilized 才是墙钟秒。"""
    cpus = vals.get("_cpus_utilized")
    if "task-clock" in vals and cpus:
        return vals["task-clock"] / 1000.0 / cpus
    return fallback


def stat_values(path):
    """perf stat --csv：value,unit,event,run_time,pct,ratio,desc"""
    vals = {}
    for line in open(path):
        if line.startswith("#") or not line.strip():
            continue
        parts = line.split(",")
        if len(parts) < 3:
            continue
        try:
            vals[parts[2]] = float(parts[0])
            if parts[2] == "task-clock" and len(parts) > 5:
                vals["_cpus_utilized"] = float(parts[5])
        except ValueError:
            continue
    return vals


def case_view(d):
    viewers = d["viewers"]
    srv = d.get("server") or {}
    cores = srv.get("cpu_cores") or 0.0
    cmds = d.get("commands")
    return {
        "id": d["id"], "viewers": viewers, "cores": cores,
        "mcpu_per_view": 1000.0 * cores / viewers,
        "pss_mib": (srv.get("pss_kib_max") or 0) / 1024.0, "fd_max": srv.get("fd_max"),
        "dut_data": artifact(cmds, ".dut.perf.data"),
        "core_data": artifact(cmds, ".cores.perf.data"),
        "stat_file": artifact(cmds, ".stat.csv") or (artifact(cmds, ".dut.perf.data") or "").replace(".dut.perf.data", ".stat.csv"),
        "window_s": (d.get("window_aligned") or {}).get("span_seconds"),
        "media": d.get("media"), "softirq": d.get("softirq_per_cpu"),
        "thread_cpu": d.get("thread_cpu_profile"), "status": d.get("status"),
    }


def main():
    a = case_view(load(SUMMARY, ID_A))
    b = case_view(load(SUMMARY, ID_B))

    print("=" * 78)
    print("1. 权威口径（perf_run 测量窗口内）")
    print(f"{'':26}{'A ' + ID_A[:22]:>26}{'B ' + ID_B[:22]:>26}")
    for key, label, fmt in [("cores", "DUT CPU 核", "{:.3f}"), ("mcpu_per_view", "mCPU/观看", "{:.3f}"),
                            ("pss_mib", "PSS 峰值 MiB", "{:.1f}"), ("fd_max", "FD 峰值", "{}"),
                            ("window_s", "窗口秒", "{:.0f}"), ("status", "状态", "{}")]:
        va, vb = a.get(key), b.get(key)
        sa = fmt.format(va) if isinstance(va, (int, float)) and key != "status" else str(va)
        sb = fmt.format(vb) if isinstance(vb, (int, float)) and key != "status" else str(vb)
        print(f"{label:26}{sa:>26}{sb:>26}")
    for c in (a, b):
        m = c.get("media") or {}
        print(f"  {c['id']}: 媒体 ready={m.get('ready')} progressing={m.get('progressing')} "
              f"failed={m.get('failed')} runtime_failures={m.get('runtime_failures')}")
    print(f"{'差值 mCPU/观看 (B-A)':26}{b['mcpu_per_view'] - a['mcpu_per_view']:>52.3f}")

    print("\n" + "=" * 78)
    print("2. DSO 归属换算成 mCPU/观看（0.001% 阈值，完整覆盖）")
    res = {}
    for c in (a, b):
        res[c["id"]] = dso_attribution(c["dut_data"])
        total = sum(res[c["id"]].values())
        k = sum(v for dso, v in res[c["id"]].items() if dso.startswith(KERNEL_DSOS))
        c["kernel_pct"] = 100.0 * k / total if total else 0.0
        c["user_pct"] = 100.0 - c["kernel_pct"]
    print(f"{'画像内核态占比 %':24}{a['kernel_pct']:>25.2f}{b['kernel_pct']:>26.2f}")
    print(f"{'画像用户态占比 %':24}{a['user_pct']:>25.2f}{b['user_pct']:>26.2f}")
    print(f"{'（内核 = kernel.kallsyms/vmlinux/vdso）':24}")
    da, db = res[a["id"]], res[b["id"]]
    keys = sorted(set(da) | set(db),
                  key=lambda k: -abs(da.get(k, 0) * a["mcpu_per_view"] / 100 -
                                     db.get(k, 0) * b["mcpu_per_view"] / 100))
    print(f"\n{'DSO':34}{'A':>10}{'B':>10}{'差值':>10}")
    for k in keys[:12]:
        va = da.get(k, 0.0) / 100.0 * a["mcpu_per_view"]
        vb = db.get(k, 0.0) / 100.0 * b["mcpu_per_view"]
        print(f"{k[:33]:34}{va:>10.3f}{vb:>10.3f}{vb - va:>+10.3f}")

    print("\n" + "=" * 78)
    print(f"3. self 热点差值 top 25（mCPU/观看，阈值 {SELF_LIMIT}%）")
    sa, sb_ = self_symbols(a["dut_data"]), self_symbols(b["dut_data"])
    rows = []
    for k in set(sa) | set(sb_):
        va = sa.get(k, 0.0) / 100.0 * a["mcpu_per_view"]
        vb = sb_.get(k, 0.0) / 100.0 * b["mcpu_per_view"]
        rows.append((vb - va, va, vb, k[1], k[0], k[2]))
    rows.sort(key=lambda r: -abs(r[0]))
    print(f"{'符号':44}{'A':>8}{'B':>8}{'差值':>9}  DSO")
    for d, va, vb, dso, sym, ku in rows[:25]:
        print(f"{sym[:43]:44}{va:>8.3f}{vb:>8.3f}{d:>+9.3f}  {ku} {dso[:16]}")

    print("\n" + "=" * 78)
    print("4. perf stat（每观看每秒）")
    for c in (a, b):
        if not c["stat_file"] or not os.path.exists(c["stat_file"]):
            print(f"  {c['id']}: 无 stat 文件")
            continue
        st = stat_values(c["stat_file"])
        dur = stat_window(st, c["window_s"] or 1)
        print(f"--- {c['id']}（stat 窗 {dur:.1f}s，非整测量窗）---")
        for name in ("cycles", "instructions", "context-switches", "cpu-migrations", "page-faults",
                     "syscalls:sys_enter_sendmsg", "syscalls:sys_enter_sendto",
                     "syscalls:sys_enter_sendmmsg", "syscalls:sys_enter_writev",
                     "syscalls:sys_enter_write", "syscalls:sys_enter_recvmsg",
                     "syscalls:sys_enter_recvfrom", "syscalls:sys_enter_read",
                     "syscalls:sys_enter_epoll_wait", "syscalls:sys_enter_futex"):
            v = st.get(name)
            if v:
                print(f"  {name:36}{v / dur / c['viewers']:>12.2f} /观看/s")
        if st.get("cycles"):
            print(f"  {'IPC':36}{st.get('instructions', 0) / st['cycles']:>12.2f}")

    print("\n" + "=" * 78)
    print("5. 线程 CPU 与 softirq")
    for c in (a, b):
        tc = c.get("thread_cpu") or {}
        if tc:
            vals = sorted((v for v in tc.values()), reverse=True)
            print(f"--- {c['id']}: 线程 {len(tc)} 个，活跃 {sum(1 for v in vals if v > 0.01)} 个，"
                  f"合计 {sum(vals):.2f} CPU-s，最热 {vals[0]:.3f}s")
        rows = c.get("softirq") or {}
        if rows:
            dur = c["window_s"] or 1
            top = sorted(((sum(v[:8]) / 8 / dur, n) for n, v in rows.items() if sum(v[:8])),
                         reverse=True)[:5]
            print(f"    softirq top5（服务端 8 核均值 /s）: " +
                  ", ".join(f"{n}={r:.0f}" for r, n in top))


main()
