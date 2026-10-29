#!/usr/bin/env python3
"""阶段 6 标定统计复核：区分两种估计量、给出配对级证据与置信区间。

定义（全文统一）：
- 原始观测 = perf_run 在测量窗口内测得的 DUT 进程 CPU（权威口径），单位核。
- 中心值 = 同一基线该档位全部有效运行的中位数（median）。n=3。
- Δ_center = median(B) - median(A)：先聚合再求差。
- Δ_pair  = median over 配对 i of (B_i - A_i)：先逐对求差再聚合。
- 相对值  = Δ / median(A)。两种 Δ 对应两种相对值，必须同时标注。
- 配对内相对值 = (B_i - A_i) / A_i，再取中位（避免个别高负载配对主导绝对差）。
- 波动 = 同臂重复的极差（max-min）与平均绝对偏差（MAD）。
- 配对差 95% 区间 = mean(d) ± t(0.975, n-1) · s(d)/√n（n=3 时 t=4.303，区间必然很宽）。
"""
import json
import pathlib
import statistics as st
import sys

T_STUDENT = {1: 12.706, 2: 4.303, 3: 3.182, 4: 2.776}   # 95% 双侧，df = n-1
PRACTICAL = 0.02                                # 工程阈值：2% 以下不视为容量级差异

files = [pathlib.Path(p) for p in sys.argv[1:]]
rows = [json.loads(l) for f in files for l in f.read_text().splitlines() if l.strip()]
all_rows = [r for r in rows if r.get("classification") == "PASS"]
rows = [r for r in all_rows if r.get("baseline") in ("A", "B")]

tiers = {}
for r in rows:
    tiers.setdefault(r["tier"], {}).setdefault(r["pair_slot"][0], {})[r["baseline"]] = r

cpu = lambda r: (r.get("server") or {}).get("cpu_cores")

print("# 阶段 6 标定统计复核\n")
print(f"有效样本：{len(rows)} 条（仅 classification=PASS 且基线为 A/B）\n")

summary = []
for tier, pairs in tiers.items():
    a = [p["A"] for p in pairs.values() if "A" in p and cpu(p["A"])]
    b = [p["B"] for p in pairs.values() if "B" in p and cpu(p["B"])]
    if len(a) < 2 or len(b) < 2:
        continue
    print(f"## {tier}\n")
    print("原始观测（核，按配对顺序）：")
    for k in sorted(pairs):
        pa = cpu(pairs[k].get("A")) if "A" in pairs[k] else None
        pb = cpu(pairs[k].get("B")) if "B" in pairs[k] else None
        slot = f"{pairs[k].get('A', pairs[k].get('B'))['pair_slot']}"
        if pa and pb:
            print(f"  配对 {k} (slot {slot}): A={pa:.4f}  B={pb:.4f}  d={pb-pa:+.4f} ({(pb-pa)/pa*100:+.2f}%)")
        else:
            print(f"  配对 {k} (slot {slot}): A={'-' if pa is None else format(pa,'.4f')}  B={'-' if pb is None else format(pb,'.4f')}  无法配对")
    av, bv = [cpu(r) for r in a], [cpu(r) for r in b]
    ma, mb = st.median(av), st.median(bv)
    d_center = mb - ma
    ds = [cpu(p["B"]) - cpu(p["A"]) for p in pairs.values() if "A" in p and "B" in p]
    rels = [(cpu(p["B"]) - cpu(p["A"])) / cpu(p["A"]) for p in pairs.values() if "A" in p and "B" in p]
    d_pair = st.median(ds) if ds else None
    d_pair_rel = st.median(rels) if rels else None
    spread = max(max(av) - min(av), max(bv) - min(bv))
    mad = max(st.mean([abs(x - ma) for x in av]), st.mean([abs(x - mb) for x in bv]))
    print()
    print(f"中心值：A median={ma:.4f}（n={len(av)}，极差 {max(av)-min(av):.4f}）  "
          f"B median={mb:.4f}（n={len(bv)}，极差 {max(bv)-min(bv):.4f}）")
    print(f"Δ_center（先聚合再求差）= {d_center:+.4f} 核（{d_center/ma*100:+.2f}%）")
    if ds:
        signs = {d > 0 for d in ds if abs(d) > 1e-9}
        md = st.mean(ds)
        sd = st.stdev(ds) if len(ds) > 1 else 0.0
        ci = T_STUDENT.get(len(ds) - 1) * sd / len(ds) ** 0.5 if len(ds) > 1 else float("inf")
        print(f"Δ_pair（先逐对再聚合）  = {d_pair:+.4f} 核（相对中位 {d_pair_rel*100:+.2f}%）")
        print(f"配对差序列 = {[f'{d:+.4f}' for d in ds]}  符号一致={'是' if len(signs) <= 1 else '否（方向不一致）'}")
        print(f"配对差均值={md:+.4f}  s={sd:.4f}  95%CI=[{md-ci:+.4f}, {md+ci:+.4f}]（t={T_STUDENT.get(len(ds)-1)}, df={len(ds)-1}）")
    print(f"同臂波动：极差上界={spread:.4f}  MAD 上界={mad:.4f}")
    crosses_zero = any(x <= 0 <= y or y <= 0 <= x for x, y in [(md - ci, md + ci)]) if ds else True
    if not ds or crosses_zero:
        verdict = "①差值未超过噪声（配对差 95% 区间覆盖 0，不能判定存在差异）"
    elif abs(d_pair_rel) < PRACTICAL:
        verdict = "②超过噪声但不足以证明生产代码变化（区间不覆盖 0，但幅度低于工程阈值且存在客户端混淆）"
    else:
        verdict = "③有统计支持的系统性差异"
    print(f"判定：{verdict}\n")
    summary.append((tier, ma, mb, d_center, d_center / ma, d_pair, d_pair_rel, spread, mad,
                    "覆盖 0" if crosses_zero else "不覆盖 0", verdict[0]))

print("## 汇总\n")
print(f"{'档位':16s} {'A中心':>8} {'B中心':>8} {'Δ_center':>10} {'相对':>7} {'Δ_pair':>9} {'配对相对':>8} "
      f"{'同臂极差':>8} {'CI':>8} {'判定':>4}")
for t, ma, mb, dc, dcr, dp, dpr, sp, mad, cz, v in summary:
    print(f"{t:16s} {ma:8.4f} {mb:8.4f} {dc:+10.4f} {dcr*100:+7.2f}% {dp:+9.4f} {dpr*100:+8.2f}% "
          f"{sp:8.4f} {cz:>8} {v:>4}")

print("\n## 交叉替换（客户端混淆幅度）\n")
swap = {}
for r in all_rows:
    if r.get("tier") in ("whep-s1-1200", "whep-s1-1200-swap"):
        swap.setdefault(r["baseline"], []).append(cpu(r))
for k in sorted(swap):
    v = [x for x in swap[k] if x]
    print(f"{k:16s} median={st.median(v):.4f}  n={len(v)}  值={[f'{x:.4f}' for x in sorted(v)]}")
if all(k in swap for k in ("A", "B", "A+B-client", "B+A-client")):
    m = lambda k: st.median([x for x in swap[k] if x])
    print(f"\n服务端净效应（客户端固定在 A）: {(m('B+A-client')-m('A'))/m('A')*100:+.2f}%")
    print(f"服务端净效应（客户端固定在 B）: {(m('B')-m('A+B-client'))/m('A+B-client')*100:+.2f}%")
    print(f"客户端净效应（服务端固定在 A）: {(m('A+B-client')-m('A'))/m('A')*100:+.2f}%")
    print(f"客户端净效应（服务端固定在 B）: {(m('B')-m('B+A-client'))/m('B+A-client')*100:+.2f}%")
    print("两个方向的服务端/客户端效应不可分离（混合组合 n=2），只能给出总差异的上界归因。")
