#!/usr/bin/env python3
"""One markdown table row per perf_run directory (result.json or result.json.gz)."""
import gzip
import json
import sys
from pathlib import Path

print("| 轮次 | 结果 | 进展 | 实时比 min | 滞后 p50/max ms | 服务端 Gcyc/s | 每路 Mcyc/s | 最忙 CPU | 服务端丢包 | 控制 p99 ms | 首帧 p50/p99 ms | perf 每路 Mcyc/s | IPC | PSS MiB | FD | 清理 | 音画偏差 ms |")
print("| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |")
for argument in sys.argv[1:]:
    run = Path(argument)
    plain = run / "result.json"
    packed = run / "result.json.gz"
    if not plain.exists() and not packed.exists():
        continue
    result = json.loads(plain.read_text() if plain.exists() else gzip.decompress(packed.read_bytes()))
    groups = result.get("groups", [])
    measured = [group["measurement"] for group in groups if group.get("measurement")]
    requested = sum(group.get("requested", 0) for group in groups)
    progressing = sum(item.get("progressing", 0) for item in measured)
    realtime = min((item.get("viewer_realtime_min", 0) for item in measured), default=0)
    lag_p50 = max((item.get("viewer_lag_span_ms_p50", 0) for item in measured), default=0)
    lag_max = max((item.get("viewer_lag_span_ms_max", 0) for item in measured), default=0)
    first = [group.get("first_media") or {} for group in groups]
    first_p50 = max((item.get("first_media_ms_p50", 0) for item in first), default=0)
    first_p99 = max((item.get("p99", 0) for item in first), default=0)
    cpu = result.get("cpu_busy", {})
    gcycles = cpu.get("server_gcycles_per_second", 0)
    sources = result.get("config", {}).get("sources") or requested
    per_session = 1000 * gcycles / sources if sources else 0
    server = result.get("process_metrics", {}).get("server", {})
    pss = server.get("pss_kib_max", 0) / 1024
    perf = result.get("perf_stat") or {}
    perf_session = perf.get("cycles", 0) / perf["seconds"] / sources / 1e6 if perf.get("seconds") and sources else 0
    cleanup = result.get("cleanup") or {}
    cleaned = "ok" if not cleanup.get("remaining_owned_receivers") and cleanup.get("all_owned_processes_reaped") else "残留"
    probe = result.get("control_probe") or {}
    av = " ".join(f"{protocol}:{item.get('av_offset_ms_min', 0):.0f}/{item.get('av_offset_ms_max', 0):.0f}"
                  f"{'' if item.get('ok') else '!'}{' 回退' if item.get('video_pts_backward') or item.get('audio_pts_overlaps') else ''}"
                  for protocol, item in (result.get("av_probe") or {}).items())
    print(f"| {run.name} | {result.get('status')}/{result.get('capacity_boundary')} | {progressing}/{requested} | {realtime:.3f} | {lag_p50}/{lag_max} | "
          f"{gcycles:.2f} | {per_session:.1f} | {cpu.get('server_max_cpu_busy', 0):.2f} | {result.get('server_evidence', {}).get('udp_drops', '')} | "
          f"{probe.get('ms_p99', 0):.1f} | {first_p50}/{first_p99} | {perf_session:.1f} | {perf.get('ipc', 0):.2f} | {pss:.0f} | {server.get('fd_max', '')} | {cleaned} | {av} |")
