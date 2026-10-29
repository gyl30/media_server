#!/usr/bin/env python3
"""阶段 6 P0 画像分析：把 perf/bpftrace 原始数据转成可核对的分层归因。

用法：
  pi_phase6r.py self <case.stem>            # self 热点，按用户态/内核态分列，含 DSO
  pi_phase6r.py callers <case.stem> <sym>   # 指定符号的调用者链
  pi_phase6r.py cores <case.stem>           # 服务端 8 核上"谁在跑内核代码"
  pi_phase6r.py diff <stemA> <stemB>        # 两个档位的画像差分
  pi_phase6r.py stat <case.stem>            # perf stat 汇总（IPC、上下文切换、syscall 速率）
  pi_phase6r.py offcpu <case.stem>          # bpftrace 直方图转成加权 µs
  pi_phase6r.py table <summary.jsonl>       # 各档位 CPU 分解与 profiler 干扰对照
"""
import json
import re
import subprocess
import sys


def run(cmd):
    return subprocess.run(cmd, capture_output=True, text=True, timeout=900).stdout


SELF = ["perf", "report", "--stdio", "--no-children", "-g", "none", "--percent-limit", "0.1"]
LINE = re.compile(r"^\s+([0-9.]+)%\s+(\S+)\s+(\S+)\s+\[([k.])\]\s+(.+)$")


def parse_self(stem, extra=None):
    cmd = SELF + ["-i", f"{stem}.dut.perf.data"] + (extra or [])
    out = {"user": [], "kernel": [], "total_pct": 0.0}
    for line in run(cmd).splitlines():
        m = LINE.match(line)
        if not m:
            continue
        pct, comm, dso, kind, sym = float(m[1]), m[2], m[3], m[4], m[5]
        out["total_pct"] += pct
        out["kernel" if kind == "k" else "user"].append({"pct": pct, "comm": comm, "dso": dso, "sym": sym})
    return out


def show_self(stem):
    d = parse_self(stem)
    print(f"== {stem} self 热点（覆盖 {d['total_pct']:.1f}% 样本）==")
    for kind in ("user", "kernel"):
        rows = d[kind]
        tot = sum(r["pct"] for r in rows)
        print(f"\n[{kind}] 合计 {tot:.2f}%（{len(rows)} 项，阈值 0.1%）")
        for r in rows[:18]:
            print(f"  {r['pct']:6.2f}%  {r['dso'][:28]:28s} {r['sym'][:88]}")


def show_callers(stem, sym):
    cmd = ["perf", "report", "-i", f"{stem}.dut.perf.data", "--stdio", "--symbols", sym, "-q",
           "-g", "graph,0.5,caller", "--percent-limit", "0.2"]
    txt = run(cmd)
    keep, started = [], False
    for line in txt.splitlines():
        if line.startswith("#"):
            started = True
            continue
        if started:
            keep.append(line)
    print("\n".join(keep[:70]))


def show_cores(stem):
    cmd = ["perf", "report", "-i", f"{stem}.cores.perf.data", "--stdio", "--no-children", "-g", "none",
           "--sort", "comm,dso", "--percent-limit", "0.3"]
    print(run(cmd)[-4000:])


def show_diff(a, b):
    cmd = ["perf", "diff", "-i", f"{a}.dut.perf.data", "-i", f"{b}.dut.perf.data", "--stdio"]
    print(run(cmd)[:9000])


def show_stat(stem):
    """perf stat 汇总。速率按墙钟秒换算：perf 的 /sec 以 task-clock 为分母（多线程会低估）。"""
    rows = {}
    for line in open(stem + ".stat.csv"):
        if line.startswith("#") or not line.strip():
            continue
        c = line.rstrip().split(",")
        if len(c) < 7:
            continue
        try:
            rows[c[2]] = {"value": float(c[0]), "enabled": float(c[4]) or 100.0, "cpus": float(c[5]) if c[5] else 0.0}
        except ValueError:
            continue
    tc = rows.get("task-clock", {}).get("value", 0.0)
    cpus = rows.get("task-clock", {}).get("cpus", 0.0)
    if not tc or not cpus:
        print(f"== {stem} perf stat ==（无 task-clock，跳过）")
        return
    wall = tc / 1000.0 / cpus
    print(f"== {stem} perf stat（墙钟 {wall:.1f} s，平均 {cpus:.2f} 核）==")
    cyc, ins = rows.get("cycles", {}).get("value"), rows.get("instructions", {}).get("value")
    if cyc and ins:
        print(f"IPC = {ins/cyc:.2f}   有效频率 = {cyc/wall/1e9:.2f} GHz   使能率 {rows['cycles']['enabled']:.0f}%")
    for k in ("context-switches", "page-faults"):
        if k in rows:
            print(f"{k:20s} {rows[k]['value']/wall:12.0f} /s")
    sysl = {k: v for k, v in rows.items() if k.startswith("syscalls:") and v["value"]}
    if sysl:
        print("syscall（墙钟每秒）:")
        for k, v in sorted(sysl.items(), key=lambda x: -x[1]["value"]):
            print(f"  {k.split('_enter_')[-1]:16s} {v['value']/wall:12.0f}")


BUCKET = re.compile(r"^\[\s*([0-9.]+[KMG]?),\s*([0-9.]+[KMG]?)\)\s+(\d+)\s+\|")


def to_us(v):
    mult = {"K": 1e3, "M": 1e6, "G": 1e9}
    return float(v[:-1]) * mult[v[-1]] if v and v[-1] in mult else float(v)


def show_offcpu(stem):
    txt = open(stem + ".offcpu.txt").read()
    cur, out = None, {}
    for line in txt.splitlines():
        if line.startswith("@") and line.rstrip().endswith(":"):
            cur = line.split(":")[0].lstrip("@").strip()
            out[cur] = []
            continue
        m = BUCKET.match(line)
        if m and cur:
            lo, hi, n = to_us(m[1]), to_us(m[2]), int(m[3])
            out[cur].append((lo, hi, n))
    print(f"== {stem} off-CPU ==")
    for k, buckets in out.items():
        if not buckets:
            continue
        n = sum(b[2] for b in buckets)
        mid = sum((lo + hi) / 2 * c for lo, hi, c in buckets)
        p50 = next(((lo + hi) / 2 for lo, hi, c in buckets if sum(x[2] for x in buckets if x[0] < (lo + hi) / 2) >= n / 2), None)
        print(f"{k:16s} 次数={n:8d} 平均={mid/n/1e3 if n else 0:8.2f} ms 中位≈{(p50 or 0)/1e3:8.2f} ms "
              f"总时长={mid/1e6:8.2f} s 占线程时间={mid/1e6/53.0/16*100:5.1f}%")
    if not any(out.values()):
        print("(无直方图数据)")


def absdiff(a, b, ca, cb):
    """按绝对核数比较两个档位的函数开销：占比 × 该档位权威 CPU，避免相对占比掩盖真实增量。"""
    def load(stem, cores):
        out = {}
        for line in run(SELF + ["-i", f"{stem}.dut.perf.data", "--percent-limit", "0.02"]).splitlines():
            m = LINE.match(line)
            if m:
                key = (m[3], m[5])
                out[key] = out.get(key, 0.0) + float(m[1]) / 100.0 * cores
        return out
    da, db = load(a, float(ca)), load(b, float(cb))
    keys = set(da) | set(db)
    rows = [(db.get(k, 0.0) - da.get(k, 0.0), k, da.get(k, 0.0), db.get(k, 0.0)) for k in keys]
    rows.sort(key=lambda r: -abs(r[0]))
    print(f"{'Δ核数':>10} {'A核数':>9} {'B核数':>9}  DSO / 符号")
    for d, k, x, y in rows[:22]:
        if abs(d) < 0.0005:
            continue
        print(f"{d:+10.4f} {x:9.4f} {y:9.4f}  {k[0][:22]:22s} {k[1][:74]}")
    print(f"\n合计 A={sum(da.values()):.3f} 核  B={sum(db.values()):.3f} 核（仅统计 >=0.02% 条目）")


def show_table(path):
    rows = [json.loads(l) for l in open(path) if l.strip()]
    tiers = {}
    for r in rows:
        if r.get("tier"):
            tiers.setdefault(r["tier"], []).append(r)
    print(f"{'档位':16s} {'模式':6s} {'CPU':>7} {'user':>7} {'system':>7} {'u%':>5} {'8核busy':>8} "
          f"{'sirq/核':>8} {'客户端CPU':>9} {'媒体':>10} {'窗口内':>7}")
    for t, rs in tiers.items():
        for r in rs:
            wa = r.get("window_aligned") or {}
            cpu = (r.get("server") or {}).get("cpu_cores")
            u, s = wa.get("dut_user_cores"), wa.get("dut_system_cores")
            pc = (wa.get("per_core") or {})
            busy = [pc.get(f"cpu{i}", {}).get("busy") for i in range(8)]
            sirq = [pc.get(f"cpu{i}", {}).get("softirq") for i in range(8)]
            busy = sum(x for x in busy if x is not None) / 8 if all(x is not None for x in busy) else None
            sirq = sum(x for x in sirq if x is not None) / 8 if all(x is not None for x in sirq) else None
            m = r.get("media") or {}
            media = f"{m.get('ready')}/{m.get('progressing')}"
            inside = all((r.get("windows_inside") or {}).values()) if r.get("windows_inside") else "-"
            print(f"{t:16s} {str(r.get('mode')):6s} {cpu if cpu else 0:7.3f} {u if u else 0:7.3f} "
                  f"{s if s else 0:7.3f} {(u/cpu*100) if u and cpu else 0:5.1f} "
                  f"{busy if busy else 0:8.4f} {sirq if sirq else 0:8.4f} {r.get('client_cpu_sum') or 0:9.3f} "
                  f"{media:>10} {str(inside):>7}")
    print("\n== profiler 干扰（同档位 prof - plain）==")
    for t, rs in tiers.items():
        pl = [r for r in rs if r.get("mode") == "plain"]
        pr = [r for r in rs if r.get("mode") == "prof"]
        if not pl or not pr:
            continue
        a = (pl[0].get("server") or {}).get("cpu_cores")
        b = sum((r.get("server") or {}).get("cpu_cores") for r in pr) / len(pr)
        ua = (pl[0].get("window_aligned") or {}).get("dut_user_cores", 0) + (pl[0].get("window_aligned") or {}).get("dut_system_cores", 0)
        ub = sum((r.get("profile_span_cpu") or {}).get("dut_user_cores", 0) + (r.get("profile_span_cpu") or {}).get("dut_system_cores", 0) for r in pr) / len(pr)
        print(f"{t:16s} 权威口径 {a:.3f} → {b:.3f} ({(b-a)/a*100:+.2f}%)   独立采样 {ua:.3f} → {ub:.3f} ({(ub-ua)/ua*100:+.2f}%)")
    print("\n== 线程 CPU 分布（prof 运行，服务端 8 worker 之外是否显著）==")
    for r in rows:
        tc = r.get("thread_cpu_profile")
        if not tc:
            continue
        vals = sorted(tc.values(), reverse=True)
        print(f"{r['id']:22s} 线程数={len(vals):3d} 最大={vals[0]:.3f} 中位={vals[len(vals)//2]:.3f} "
              f"尾部3={vals[-3:]}")


CMDS = {"self": (show_self, 1), "callers": (show_callers, 2), "cores": (show_cores, 1),
        "diff": (show_diff, 2), "stat": (show_stat, 1), "offcpu": (show_offcpu, 1), "table": (show_table, 1), "absdiff": (absdiff, 4)}

if __name__ == "__main__":
    fn, n = CMDS[sys.argv[1]]
    fn(*sys.argv[2:2 + n])
