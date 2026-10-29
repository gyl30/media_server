#!/usr/bin/env python3
"""off-CPU 采集辅助进程，必须在网络命名空间之外运行（ip netns exec 的新挂载视图里看不到 tracefs）。

轮询 <out_dir>/*.offcpu.request（内容 {"tgid":..,"secs":..}），出现即运行 bpftrace 并把实际
起止时间与输出写入 <out_dir>/<id>.offcpu.txt / <out_dir>/<id>.offcpu.meta.json。
用法：pi_phase6o.py <out_dir> [退出标记文件]
"""
import json
import pathlib
import subprocess
import sys
import time

OUT = pathlib.Path(sys.argv[1]).resolve()
DONE = pathlib.Path(sys.argv[2]) if len(sys.argv) > 2 else None
seen = set()

while True:
    for req in sorted(OUT.glob("*.offcpu.request")):
        if req in seen:
            continue
        seen.add(req)
        spec = json.loads(req.read_text())
        stem = req.name[: -len(".offcpu.request")]
        bt = OUT / f"{stem}.offcpu.bt"
        out = OUT / f"{stem}.offcpu.txt"
        meta = {"script": str(bt), "tgid": spec["tgid"], "secs": spec["secs"]}
        t0 = time.monotonic_ns()
        with out.open("w") as fh:
            proc = subprocess.Popen(["bpftrace", str(bt)], stdout=fh, stderr=subprocess.STDOUT,
                                    stdin=subprocess.DEVNULL)
            rc = proc.wait()
        meta.update({"start_monotonic_ns": t0, "end_monotonic_ns": time.monotonic_ns(), "exit_code": rc})
        (OUT / f"{stem}.offcpu.meta.json").write_text(json.dumps(meta) + "\n")
        print(f"[offcpu] {stem} tgid={spec['tgid']} secs={spec['secs']} rc={rc}", flush=True)
    if DONE is not None and DONE.is_file():
        break
    time.sleep(0.2)
