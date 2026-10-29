#!/usr/bin/env python3
"""查看窗口内 PSS / CPU 时间序列，判断高观看档是否存在缓冲累积或窗口内漂移。"""
import json
import os
import sys

base = sys.argv[1]
for name in sys.argv[2:]:
    r = json.load(open(os.path.join(base, name, "result.json")))
    bs = r.get("budget_samples") or []
    if not bs:
        print(name, "无 budget_samples；键:", list(r.keys()))
        continue
    k = [x for x in ("server_pss_kib", "pss_kib", "server_pss_mib") if x in bs[0]]
    ck = [x for x in ("server_cpu_cores", "cpu_cores") if x in bs[0]]
    print(name, "字段:", sorted(bs[0].keys()), "样本数:", len(bs))
    if not k:
        continue
    pss = [x["server_pss_kib"] for x in bs if x.get("server_pss_kib")]; n = len(pss)
    seg = lambda a: sum(a) / len(a) / 1024 if a else 0
    print("  PSS MiB 三段均值: %8.1f %8.1f %8.1f  (窗口内增长 %+.1f)" %
          (seg(pss[:n//3]), seg(pss[n//3:2*n//3]), seg(pss[2*n//3:]), seg(pss[2*n//3:]) - seg(pss[:n//3])))
    if not ck:
        continue
    pss, cpu = [s[k[0]] for s in bs], [s[ck[0]] for s in bs]
    n = len(pss)
    seg = lambda a: sum(a) / len(a) if a else 0
    print("  PSS MiB 三段均值: %8.1f %8.1f %8.1f   CPU 三段均值: %6.3f %6.3f %6.3f" %
          (seg(pss[:n // 3]) / 1024, seg(pss[n // 3:2 * n // 3]) / 1024, seg(pss[2 * n // 3:]) / 1024,
           seg(cpu[:n // 3]), seg(cpu[n // 3:2 * n // 3]), seg(cpu[2 * n // 3:])))
