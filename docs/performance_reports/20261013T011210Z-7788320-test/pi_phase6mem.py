#!/usr/bin/env python3
"""P0 §2.1 修正：把 PSS 时间序列按"建立期 / 稳定期"分开，避免混用归一化口径。

用法：pi_phase6mem.py <run_dir> <case> [<case> ...]
"""
import json
import os
import sys


def slope(points):
    """最小二乘斜率（MiB/s）。points: [(秒, MiB)]"""
    n = len(points)
    if n < 3:
        return 0.0
    mx = sum(p[0] for p in points) / n
    my = sum(p[1] for p in points) / n
    den = sum((p[0] - mx) ** 2 for p in points)
    if den == 0:
        return 0.0
    return sum((p[0] - mx) * (p[1] - my) for p in points) / den


def analyze(base, case):
    r = json.load(open(os.path.join(base, case, "result.json")))
    bs = [s for s in (r.get("budget_samples") or []) if s.get("server_pss_kib")]
    if not bs:
        print(case, "无 PSS 样本")
        return
    t0 = bs[0]["monotonic_ns"]
    win0 = r["requested_window"]["start_monotonic_ns"]
    win1 = r["requested_window"]["end_monotonic_ns"]
    viewers = sum(g.get("requested", 0) for g in r["groups"])
    # 各阶段样本数，判断建立期边界
    phases = {}
    for s in bs:
        phases.setdefault(s.get("phase"), [1e99, -1e99, 0])
        e = phases[s.get("phase")]
        t = (s["monotonic_ns"] - t0) / 1e9
        e[0] = min(e[0], t); e[1] = max(e[1], t); e[2] += 1
    series = [((s["monotonic_ns"] - t0) / 1e9, s["server_pss_kib"] / 1024.0) for s in bs]
    print(f"== {case}  观看={viewers}  样本={len(bs)}  总时长={series[-1][0]:.0f}s")
    for p, (a, b, n) in sorted(phases.items(), key=lambda x: x[1][0]):
        print(f"   phase={p:12s} {a:7.1f}..{b:7.1f}s  n={n}")
    w0 = (win0 - t0) / 1e9
    w1 = (win1 - t0) / 1e9
    print(f"   测量窗口 {w0:.1f}..{w1:.1f}s")
    at = lambda t: min(series, key=lambda p: abs(p[0] - t))[1]
    print("   PSS: 起始 %.0f MiB | 窗口起点 %.0f | 窗口 25%% %.0f | 50%% %.0f | 75%% %.0f | 末 %.0f MiB"
          % (series[0][1], at(w0), at(w0 + (w1 - w0) * .25), at(w0 + (w1 - w0) * .5),
             at(w0 + (w1 - w0) * .75), series[-1][1]))
    pre = [p for p in series if p[0] < w0]
    post = [p for p in series if w0 + (w1 - w0) * 0.5 <= p[0] <= w1]
    s_pre, s_post = slope(pre), slope(post)
    print("   斜率：建立期 %+.2f MiB/s（累计 %+.0f MiB，%+.3f MiB/观看）| 稳定期后半 %+.3f MiB/s（%+.3f MiB/观看/分钟）"
          % (s_pre, pre[-1][1] - pre[0][1] if len(pre) > 1 else 0,
             (pre[-1][1] - pre[0][1]) / viewers if len(pre) > 1 else 0,
             s_post, s_post * 60 / viewers))
    tail = [p for p in series if p[0] > w1]
    if tail:
        print("   窗口后 %+.2f MiB/s，%d 样本" % (slope(tail), len(tail)))
    # 稳定窗口内的队列与丢包计数（来自 perf_run 自身的逐秒采样）
    sm = r.get("samples") or []
    rows = [s["server"] for s in sm if isinstance(s, dict) and s.get("server")]
    if rows:
        f = lambda k: [x.get(k) or 0 for x in rows]
        snmp = lambda k: [((x.get("namespace_snmp") or {}).get(k) or 0) for x in rows]
        print("   稳定窗口内：RSS %.0f→%.0f MiB  VmSize %.0f→%.0f MiB  线程 %d→%d  "
              "UDP 队列 %d/%d B  TCP 队列 %d/%d B" %
              (f("rss_kib")[0] / 1024, f("rss_kib")[-1] / 1024,
               f("vmsize_kib")[0] / 1024, f("vmsize_kib")[-1] / 1024,
               f("thread_count")[0], f("thread_count")[-1],
               max(f("udp_tx_queue_bytes")), max(f("udp_rx_queue_bytes")),
               max(f("tcp_tx_queue_bytes")), max(f("tcp_rx_queue_bytes"))))
        d = lambda k: (snmp(k)[-1] - snmp(k)[0]) if len(snmp(k)) > 1 else 0
        print("   窗口内计数增量：UdpRcvbufErrors=%d UdpSndbufErrors=%d UdpInErrors=%d TcpRetransSegs=%d"
              % (d("UdpRcvbufErrors"), d("UdpSndbufErrors"), d("UdpInErrors"), d("TcpRetransSegs")))
        print("   RSS 斜率 %.3f MiB/s（窗口内）" % slope([((x["monotonic_ns"] - t0) / 1e9, x["rss_kib"] / 1024.0) for x in rows]))


if __name__ == "__main__":
    for c in sys.argv[2:]:
        analyze(sys.argv[1], c)
