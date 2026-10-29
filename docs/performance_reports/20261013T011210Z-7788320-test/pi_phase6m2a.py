#!/usr/bin/env python3
"""P0-M 分析：按 FD 数划分建立/稳态/清理阶段，量化每观看端内存、稳态斜率与清理后回落。

用 FD 数而不是时间标记来划分阶段：每个 WHEP 观看端对应一个客户端连接，FD 数上升即建连、
下降即关闭，因此阶段边界来自被测系统本身，不依赖驱动脚本的时钟。

用法：pi_phase6m2a.py <out_dir>
"""
import csv
import json
import pathlib
import statistics
import sys

OUT = pathlib.Path(sys.argv[1]).resolve()
rows = list(csv.DictReader((OUT / "samples.csv").open()))
result = json.loads((OUT / "result.json").read_text()) if (OUT / "result.json").is_file() else {}
if len(rows) < 5:
    raise SystemExit("样本不足")

for r in rows:
    for k in r:
        try:
            r[k] = float(r[k])
        except ValueError:
            r[k] = 0.0

fds = [r["fds"] for r in rows]
base_fd = statistics.median(fds[:20])
peak_fd = max(fds)
print(f"样本 {len(rows)} 条，间隔 {rows[1]['mono'] - rows[0]['mono']:.0f}s，"
      f"跨度 {rows[-1]['mono'] - rows[0]['mono']:.0f}s")
print(f"FD 基线 {base_fd:.0f}，峰值 {peak_fd:.0f}")

# 阶段划分：FD 数相对基线的增量。阈值取小值，避免建连过程中的样本被归为空载。
LOAD_THRESHOLD = max(20.0, 0.05 * (peak_fd - base_fd))
for i, r in enumerate(rows):
    r["conn"] = max(0.0, r["fds"] - base_fd)
    r["t"] = r["mono"] - rows[0]["mono"]

segments = []
i = 0
while i < len(rows):
    level = 1 if rows[i]["conn"] > LOAD_THRESHOLD else 0
    j = i
    while j < len(rows) and ((1 if rows[j]["conn"] > LOAD_THRESHOLD else 0) == level):
        j += 1
    if j - i >= 3:
        segments.append({"load": level, "from": i, "to": j, "seconds": rows[j - 1]["t"] - rows[i]["t"]})
    i = j

print("\n" + "=" * 76)
print("阶段（按 FD 数划分）")
print(f"{'#':>3} {'负载':>6} {'时长':>8} {'PSS 起':>10} {'PSS 止':>10} {'PSS 峰':>10} {'斜率 MiB/s':>12}")
for n, s in enumerate(segments):
    seg = rows[s["from"]:s["to"]]
    pss = [r["pss_kib"] / 1024 for r in seg]
    dur = max(1.0, seg[-1]["t"] - seg[0]["t"])
    slope = (pss[-1] - pss[0]) / dur
    print(f"{n:>3} {'有负载' if s['load'] else '空载':>6}{s['seconds']:>8.0f}"
          f"{pss[0]:>10.1f}{pss[-1]:>10.1f}{max(pss):>10.1f}{slope:>12.3f}")

# 稳态：负载段后半部分
print("\n" + "=" * 76)
print("稳态（每个负载段的后半部分）")
print(f"{'段':>3} {'观看(由FD)':>10} {'时长':>7} {'PSS 均值':>10} {'PSS 斜率':>10} {'RSS 斜率':>10} "
      f"{'锯齿幅度':>9} {'周期 s':>8} {'线程':>6} {'UDP 掉包':>9} {'TCP 重传/s':>10}")
for n, s in enumerate(segments):
    if not s["load"]:
        continue
    seg = rows[s["from"] + (s["to"] - s["from"]) // 2:s["to"]]
    if len(seg) < 6:
        continue
    pss = [r["pss_kib"] / 1024 for r in seg]
    rss = [r["rss_kib"] / 1024 for r in seg]
    dur = max(1.0, seg[-1]["t"] - seg[0]["t"])
    xs = [r["t"] - seg[0]["t"] for r in seg]
    mx = statistics.mean(xs)
    my = statistics.mean(pss)
    slope = sum((x - mx) * (y - my) for x, y in zip(xs, pss)) / max(1e-9, sum((x - mx) ** 2 for x in xs))
    mry = statistics.mean(rss)
    slope_r = sum((x - mx) * (y - mry) for x, y in zip(xs, rss)) / max(1e-9, sum((x - mx) ** 2 for x in xs))
    # 锯齿：相邻局部极大值
    peaks = [(seg[k]["t"], pss[k]) for k in range(1, len(pss) - 1)
             if pss[k] > pss[k - 1] and pss[k] > pss[k + 1] and pss[k] - min(pss) > 20]
    amp = statistics.mean([p - min(pss) for _, p in peaks]) if peaks else 0.0
    periods = [b[0] - a[0] for a, b in zip(peaks, peaks[1:])] if len(peaks) > 1 else []
    drops = seg[-1]["udp_drops"] - seg[0]["udp_drops"]
    print(f"{n:>3}{statistics.median([r['conn'] for r in seg]):>10.0f}{dur:>7.0f}"
          f"{statistics.mean(pss):>10.1f}{slope:>10.3f}{slope_r:>10.3f}{amp:>9.1f}"
          f"{(statistics.median(periods) if periods else 0):>8.1f}"
          f"{statistics.median([r['threads'] for r in seg]):>6.0f}{drops:>9.0f}"
          f"{statistics.mean([r['tcp_retrans_rate'] for r in seg]):>10.2f}")

# 清理：负载段结束后的空载段
print("\n" + "=" * 76)
print("清理后回落（每个负载段之后的空载段）")
for n, s in enumerate(segments):
    if not s["load"]:
        continue
    tail = rows[s["to"]:s["to"] + 200]
    if len(tail) < 6:
        continue
    p0 = rows[s["to"] - 1]["pss_kib"] / 1024
    marks = []
    for dt in (30, 60, 120, 300, 600):
        cand = [r["pss_kib"] / 1024 for r in tail if r["t"] - tail[0]["t"] <= dt]
        marks.append(f"{dt}s={cand[-1]:.1f}" if cand else f"{dt}s=-")
    final = tail[-1]["pss_kib"] / 1024
    lf = tail[-1].get("lazy_free_kib", "")
    pd = tail[-1].get("pss_dirty_kib", "")
    extra = ""
    if lf and pd:
        extra = (f"，LazyFree {float(lf) / 1024:.1f} MiB，Pss_Dirty {float(pd) / 1024:.1f} MiB")
    print(f"段 {n} 之后观察 {tail[-1]['t'] - tail[0]['t']:.0f}s：起点 {p0:.1f} MiB → "
          + " ".join(marks) + f" → 末值 {final:.1f} MiB（回落 {100 * (p0 - final) / max(1e-9, p0):.1f}%）")
    print(f"    末值时 FD {tail[-1]['fds']:.0f}（基线 {base_fd:.0f}），线程 {tail[-1]['threads']:.0f}，"
          f"VmSize {tail[-1]['vm_size_kib'] / 1024:.0f} MiB，RSS {tail[-1]['rss_kib'] / 1024:.1f} MiB{extra}")

# 空载段水位对比（初始 vs 最终）
idle_segs = [s for s in segments if not s["load"] and s["seconds"] > 30]
if len(idle_segs) >= 2:
    a, b = idle_segs[0], idle_segs[-1]
    pa = statistics.mean([r["pss_kib"] / 1024 for r in rows[a["from"]:a["to"]]])
    pb = statistics.mean([r["pss_kib"] / 1024 for r in rows[b["from"]:b["to"]]])
    print(f"\n空载水位：首个空载段 {pa:.1f} MiB → 末个空载段 {pb:.1f} MiB（差 {pb - pa:+.1f} MiB）")

# 每观看端建立期增量：找 FD 上升段
print("\n" + "=" * 76)
print("建立期每观看端内存（FD 上升段的 PSS/FD 回归）")
for n, s in enumerate(segments):
    if not s["load"]:
        continue
    seg = rows[max(0, s["from"] - 40):s["to"]]
    seg = [r for r in seg if r["conn"] > 0]
    if len(seg) < 6:
        continue
    xs = [r["conn"] for r in seg]
    ys = [r["pss_kib"] / 1024 for r in seg]
    mx, my = statistics.mean(xs), statistics.mean(ys)
    denom = sum((x - mx) ** 2 for x in xs)
    if denom < 1:
        continue
    beta = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / denom
    print(f"段 {n}: 每观看 {beta:.3f} MiB（样本 {len(seg)}，连接数 {min(xs):.0f}→{max(xs):.0f}）")

print("\n驱动状态:", result.get("status"), "清理:", json.dumps(result.get("cleanup", {}), ensure_ascii=False))
for p in result.get("plays", []):
    print(f"  播放 {p['tag']}: 状态 {p['status']} 请求 {p['requested']} 就绪 {p['ready']} "
          f"进度中 {p['progressing']} 失败 {p['failed']} 覆盖率 {p.get('coverage')}")
