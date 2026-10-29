#!/usr/bin/env python3
"""核对 fixture 的关键帧间隔与大小，用于解释 DUT 内存的周期性锯齿。"""
import subprocess
import sys

fx = sys.argv[1]
out = subprocess.run(["ffprobe", "-select_streams", "v:0", "-show_entries",
                      "packet=pts_time,size,flags", "-of", "csv=p=0", fx],
                     capture_output=True, text=True).stdout
ks = []
for line in out.splitlines():
    parts = line.split(",")
    if len(parts) >= 3 and "K" in parts[2]:
        try:
            ks.append((float(parts[0]), int(parts[1])))
        except ValueError:
            pass
print("视频关键帧数:", len(ks))
for t, s in ks[:8]:
    print("  pts %8.2fs  %7.1f KiB" % (t, s / 1024))
if len(ks) > 1:
    gaps = [round(b[0] - a[0], 2) for a, b in zip(ks, ks[1:])]
    print("关键帧间隔(s):", gaps[:8])
    avg = sum(s for _, s in ks) / len(ks)
    print("平均关键帧 %.1f KiB" % (avg / 1024))
