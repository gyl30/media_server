#!/usr/bin/env python3
"""阶段 6 配对标定分析：同档位 A/B 交替，按配对差判定真实代码差异。

判定规则（小样本、可解释优先）：
- 噪声门限 = 同一基线 3 次重复的极差（max-min）取两臂较大者；
- 只有 |配对差中位| 同时超过该门限、且 3 个配对符号一致时，才判为真实差异；
- 顺序效应 = A 先行的配对差与 B 先行的配对差之差（交替设计可直接观察）。
"""
import json
import pathlib
import statistics as st
import sys

rows = [json.loads(l) for p in sys.argv[1:] if p.endswith(".jsonl")
        for l in pathlib.Path(p).read_text().splitlines() if l.strip()]
skipped = [r for r in rows if r.get("classification") not in (None, "PASS")]
rows = [r for r in rows if r.get("classification") in (None, "PASS")]
if skipped:
    print(f"已排除 {len(skipped)} 条非 PASS 记录：" + ", ".join(r.get("id", "?") for r in skipped) + "\n")
SRV_CORES = [f"cpu{i}" for i in range(8)]
OUT = next((pathlib.Path(a) for a in sys.argv[2:] if not a.endswith(".jsonl")), None)

METRICS = {
    "server_cpu": lambda r: (r.get("server") or {}).get("cpu_cores"),
    "dut_user": lambda r: (r.get("window_aligned") or {}).get("dut_user_cores"),
    "dut_system": lambda r: (r.get("window_aligned") or {}).get("dut_system_cores"),
    "srv_core_busy": lambda r: _core(r, "busy"),
    "srv_core_sys": lambda r: _core(r, "system"),
    "srv_core_sirq": lambda r: _core(r, "softirq"),
    "client_cpu": lambda r: r.get("client_cpu_sum"),
    "pss_mib": lambda r: ((r.get("server") or {}).get("pss_kib_max") or 0) / 1024,
}


def _core(r, field):
    """DUT 固定绑定的 8 个物理核的平均 busy/user/system/softirq 占比。"""
    pc = ((r.get("window_aligned") or {}).get("per_core")) or {}
    vals = [pc[c][field] for c in SRV_CORES if c in pc and field in pc[c]]
    return sum(vals) / len(vals) if len(vals) == len(SRV_CORES) else None


def softirq_share(r):
    """窗口内服务端 8 核 softirq 事件数及 NET_RX 占比。"""
    sq = r.get("softirq_per_cpu") or {}
    if not sq:
        return None, None
    tot = sum(sum(v[i] for i in range(8)) for v in sq.values())
    rx = sum(sq.get("NET_RX", [0] * 32)[i] for i in range(8))
    return tot, (rx / tot if tot else None)


def main():
    tiers = {}
    for r in rows:
        if r.get("tier") and r.get("baseline") in ("A", "B"):
            tiers.setdefault(r["tier"], {}).setdefault(r["pair_slot"][0], {}).setdefault(r["baseline"], []).append(r)

    print("# 阶段 6 配对标定\n")
    print("A = 76355e4，B = 7788320。每档 3 个配对，顺序交替（A-B / B-A / A-B）。\n")
    report, anomalies = [], []
    for tier, pairs in tiers.items():
        print(f"## {tier}\n")
        print(f"{'指标':14s} {'A 中位':>9} {'B 中位':>9} {'配对差':>22} {'噪声门限':>9} {'判定':>16}")
        for name, fn in METRICS.items():
            av = sorted(v for v in (fn(r) for r in pairs.get("1", {}).get("A", []) + pairs.get("2", {}).get("A", [])
                                    + pairs.get("3", {}).get("A", [])) if v is not None)
            bv = sorted(v for v in (fn(r) for r in pairs.get("1", {}).get("B", []) + pairs.get("2", {}).get("B", [])
                                    + pairs.get("3", {}).get("B", [])) if v is not None)
            if len(av) < 2 or len(bv) < 2:
                continue
            ds = []
            for p in ("1", "2", "3"):
                a = [fn(r) for r in pairs.get(p, {}).get("A", []) if fn(r) is not None]
                b = [fn(r) for r in pairs.get(p, {}).get("B", []) if fn(r) is not None]
                if a and b:
                    ds.append(st.median(b) - st.median(a))
            if not ds:
                continue
            noise = max((max(av) - st.median(av)) if len(av) > 1 else 0,
                        (max(bv) - st.median(bv)) if len(bv) > 1 else 0)
            med = st.median(ds)
            signs = {d > 0 for d in ds if abs(d) > 1e-12}
            consistent = len(signs) <= 1
            verdict = "真实差异" if abs(med) > noise and consistent and len(ds) >= 2 else "噪声内"
            rel = f"{med / st.median(av) * 100:+.1f}%" if st.median(av) else "n/a"
            print(f"{name:14s} {st.median(av):9.4f} {st.median(bv):9.4f} {med:+12.4f} ({rel:>7}) "
                  f"{noise:9.4f} {verdict:>16}")
            report.append((tier, name, st.median(av), st.median(bv), med, rel, noise, verdict))
            if verdict == "真实差异":
                anomalies.append((tier, name, med, rel))
        print()

    swaps = [r for r in rows if r.get("baseline", "").endswith("-client")]
    if swaps:
        print("## 交叉替换（区分服务端代码与压测客户端代码）\n")
        base = {}
        for r in rows:
            if r.get("tier") == "whep-s1-1200":
                base.setdefault(r["baseline"], []).append((r.get("server") or {}).get("cpu_cores"))
        for r in swaps:
            base.setdefault(r["baseline"], []).append((r.get("server") or {}).get("cpu_cores"))
        print(f"{'组合':22s} {'服务端 CPU':>12}")
        for k, v in sorted(base.items()):
            vals = [x for x in v if x]
            print(f"{k:22s} {st.median(vals):12.4f}  (n={len(vals)})")
        print("\n若 A+A 与 A+B 接近、而 B+A 与 B+B 接近，则差异来自服务端代码；反之来自客户端代码。")

    if OUT:
        OUT.write_text("\n".join(f"{t}\t{m}\t{a:.4f}\t{b:.4f}\t{d:+.4f}\t{rel}\t{n:.4f}\t{v}"
                                 for t, m, a, b, d, rel, n, v in report) + "\n")
        print(f"\n表格已写入 {OUT}")
    print(f"\n判定为真实差异的项：{len(anomalies)}")


main()
