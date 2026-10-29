#!/usr/bin/env python3
"""核对 phase6c 各次运行实际交付量，判断 CPU 变平是否为交付不足造成。"""
import glob
import json
import os
import sys

base = sys.argv[1]
for d in sorted(glob.glob(os.path.join(base, "whep-*"))):
    f = os.path.join(d, "result.json")
    if not os.path.exists(f):
        continue
    r = json.load(open(f))
    tb = tp = n = prog = 0.0
    dur = 0.0
    for g in r["groups"]:
        m = g["measurement"]
        tb += m.get("steady_media_bytes", 0)
        n += m.get("ready", 0)
        prog += m.get("progressing", 0)
        tp += m.get("media_datagrams", 0)
        dur = max(dur, m.get("duration_seconds", 0))
    if not n or not dur:
        print(d, "无有效测量")
        continue
    print(os.path.basename(d),
          "viewers=%d progressing=%d dur=%.0fs" % (n, prog, dur),
          "kB/s/view=%.1f" % (tb / n / dur / 1e3),
          "pkt/s/view=%.1f" % (tp / n / dur),
          "total_Mbps=%.0f" % (tb / dur * 8 / 1e6))
