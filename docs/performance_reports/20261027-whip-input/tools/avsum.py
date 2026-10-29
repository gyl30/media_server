#!/usr/bin/env python3
import json
import sys
from pathlib import Path

for path in sys.argv[1:]:
    run = Path(path)
    if not (run / "result.json").exists():
        continue
    result = json.loads((run / "result.json").read_text())
    print(run.name, result["status"], result.get("capacity_boundary"), "lagging", result.get("realtime_lagging_groups"))
    for protocol, probe in (result.get("av_probe") or {}).items():
        fmt = lambda key, digits=0: "-" if probe.get(key) is None else round(probe[key], digits)
        print(f"   {protocol:5} ok={probe.get('ok')} first={fmt('first_video_s', 2)}s fps={fmt('video_fps', 2)} vback={probe.get('video_pts_backward')} "
              f"vgap={probe.get('video_pts_gaps')}/{fmt('video_pts_gap_max_ms')}ms stall={probe.get('video_arrival_stalls')} "
              f"agap={probe.get('audio_pts_gaps')} aovl={probe.get('audio_pts_overlaps')} av={probe.get('av_markers')}:"
              f"{fmt('av_offset_ms_min', 1)}/{fmt('av_offset_ms_p50', 1)}/{fmt('av_offset_ms_max', 1)} warn={probe.get('decode_warnings')}")
