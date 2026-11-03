#!/usr/bin/env python3
"""Summarize a perf_run lifecycle run: status, groups, churn and source churn rounds, cleanup snapshot, WHEP A/V probe and, when the server
ran with MEDIA_SERVER_LOG_LEVEL=debug, transcoder starts and WHEP session counts from its log."""

import gzip
import json
import re
import sys
from pathlib import Path


def load(run):
    path = run / "result.json"
    return json.loads(path.read_text()) if path.exists() else json.load(gzip.open(run / "result.json.gz"))


def summarize(run):
    result = load(run)
    server = result.get("process_metrics", {}).get("server", {})
    groups = result.get("groups", [])
    out = {"run": run.name, "status": result.get("status"), "boundary": result.get("capacity_boundary"), "errors": [e.get("message") for e in result.get("errors", [])][:5],
           "groups": {g["name"]: f"{g['measurement'].get('ready')}/{g['measurement'].get('progressing')}/{g['requested']} exit={g.get('exit_code')}" for g in groups},
           "realtime_min": min((float(g["measurement"]["viewer_realtime_min"]) for g in groups if "viewer_realtime_min" in g["measurement"]), default=None),
           "churn": {"rounds": len(result.get("churn", {}).get("rounds", [])), "failed": result.get("churn", {}).get("failed_rounds")},
           "evidence": result.get("server_evidence"), "udp_rcvbuf_errors": result.get("namespace_tcp_delta", {}).get("UdpRcvbufErrors"),
           "server_pss_max_mib": round(server.get("pss_kib_max", 0) / 1024, 1), "server_fd_max": server.get("fd_max"),
           "gcycles_per_second": result.get("cpu_busy", {}).get("server_gcycles_per_second"),
           "control": {key: result.get("control_probe", {}).get(key) for key in ("requests", "ms_p50", "ms_p99", "ms_p100", "errors")} if result.get("control_probe") else None}
    rounds = result.get("source_churn", {}).get("rounds", [])
    out["source_churn"] = {"rounds": len(rounds), "failed": result.get("source_churn", {}).get("failed_rounds"),
                           "stops": sorted({r.get("publisher_stop") for r in rounds if r.get("publisher_stop")}),
                           "fd_before_after": [(r.get("server_fd_before_viewers"), r.get("server_fd_after_viewers")) for r in rounds],
                           "lingering": sum(len(r.get("viewers_lingering", [])) for r in rounds)}
    cleanup = result.get("cleanup", {})
    settled = cleanup.get("server_settled") or {}
    out["cleanup"] = {"remaining_receivers": cleanup.get("remaining_owned_receivers"), "reaped": cleanup.get("all_owned_processes_reaped"),
                      "baseline_fd": result.get("baseline", {}).get("fd"), "settled_fd": settled.get("fd"),
                      "baseline_pss_mib": round(result.get("baseline", {}).get("pss_kib", 0) / 1024, 1),
                      "settled_pss_mib": round(settled.get("pss_kib", 0) / 1024, 1)}
    whep = result.get("av_probe", {}).get("whep")
    if whep:
        out["av_whep"] = {key: whep.get(key) for key in ("status", "video_frames", "audio_frames", "audio_pts_gaps", "decode_warnings", "av_offset_ms_p50",
                                                         "av_offset_ms_max", "error") if key in whep}
    log = run / "server.log"
    if log.exists():
        text = log.read_text(errors="replace")
        count = lambda pattern: len(re.findall(pattern, text))
        out["server_log"] = {"opus_aac_starts": count(r"audio transcoder started input opus"), "aac_opus_starts": count(r"audio transcoder started input aac"),
                             "whep_started": count(r"webrtc whep session started"), "whep_shutdown": count(r"webrtc whep session shutdown"),
                             "whep_source_ended": count(r"webrtc source stream ended"), "whip_started": count(r"webrtc whip session started"),
                             "whip_shutdown": count(r"webrtc whip session shutdown"), "errors": count(r"\] \[error\] ")}
    print(json.dumps(out))


for item in sys.argv[1:]:
    summarize(Path(item))
