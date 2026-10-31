#!/usr/bin/env python3
"""Tabulate the stage D perf_run results: server cycles, instructions, busiest core, control latency, PSS, drops, WHEP receive rates and,
from the transcoders.bt trace of each run, the WHEP egresses and audio transcoders present and created between the first and last snapshot inside the measurement window
(bpftrace prints its maps once more on exit, so the final snapshot double counts and is outside the window)."""

import argparse
import gzip
import json
import re
from datetime import datetime
from pathlib import Path

CODECS = {2: "aac", 3: "opus"}


def load(path):
    gz = path / "result.json.gz"
    return json.load(gzip.open(gz)) if gz.exists() else json.loads((path / "result.json").read_text())


def snapshots(trace):
    current = None
    found = []
    for line in trace.read_text(errors="replace").splitlines():
        if line.startswith("== "):
            current = {"time": line[3:].strip(), "egress": 0, "transcoders": {}}
            found.append(current)
        elif current is not None and (match := re.match(r"@transcoder_start\[\d+, (\d+), (\d+), (\d+)\]: (\d+)", line)):
            key = f"{CODECS.get(int(match[1]), match[1])}->{CODECS.get(int(match[2]), match[2])}/{match[3]}ch"
            current["transcoders"][key] = current["transcoders"].get(key, 0) + int(match[4])
        elif current is not None and (match := re.match(r"@egress_create\[\d+\]: (\d+)", line)):
            current["egress"] += int(match[1])
    return found


def window_delta(run, trace, warmup):
    marker = run / "measurement.start"
    if not trace.exists() or not marker.exists():
        return None
    start = datetime.fromtimestamp(marker.stat().st_mtime + warmup)
    clock = lambda value: datetime.combine(start.date(), datetime.strptime(value, "%H:%M:%S").time())
    inside = [snapshot for snapshot in snapshots(trace)[:-1] if 0 <= (clock(snapshot["time"]) - start).total_seconds() <= 60]
    if len(inside) < 2:
        return None
    first, last = inside[0], inside[-1]
    keys = set(first["transcoders"]) | set(last["transcoders"])
    return {"egress": last["egress"] - first["egress"], "egress_total": last["egress"],
            "transcoders": {key: last["transcoders"].get(key, 0) - first["transcoders"].get(key, 0) for key in sorted(keys)},
            "transcoders_total": last["transcoders"]}


def row(run, traces):
    r = load(run)
    stat = r.get("perf_stat", {})
    seconds = max(stat.get("seconds", 1), 1)
    busy = r.get("cpu_busy", {})
    server = r.get("process_metrics", {}).get("server", {})
    whep = [group["measurement"] for group in r["groups"] if group["name"].startswith("whep")]
    viewers = sum(item.get("progressing", 0) for item in whep)
    window = sum(item.get("duration_seconds", 0) for item in whep[:1]) or 60
    return {"run": run.name, "status": r["status"], "gcycles_per_s": round(stat.get("cycles", 0) / seconds / 1e9, 2),
            "ginstr_per_s": round(stat.get("instructions", 0) / seconds / 1e9, 2), "server_cores": round(server.get("cpu_cores", 0), 2),
            "busiest_core": round(busy.get("server_max_cpu_busy", 0), 3), "control_p99_ms": (r.get("control_probe") or {}).get("ms_p99"),
            "pss_mib": round(server.get("pss_kib_max", 0) / 1024, 1), "udp_drops": r.get("server_evidence", {}).get("udp_drops"),
            "tcp_retrans": r.get("namespace_tcp_delta", {}).get("TcpRetransSegs"),
            "whep_viewers": viewers, "whep_kbps_per_viewer": round(sum(item.get("received_bytes", 0) for item in whep) * 8 / 1000 / window / viewers, 1)
            if viewers else None,
            "whep_audio_pps_per_viewer": round(sum(item.get("audio_packets", 0) for item in whep) / window / viewers, 1) if viewers else None,
            "whep_unprotect_failures": sum(item.get("unprotect_failures", 0) for item in whep),
            "trace": window_delta(run, traces / f"{run.name}.bt", r["config"].get("warmup", 0))}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("runs", type=Path, help="perf_run output root (runs4)")
    parser.add_argument("--pattern", default="ab-*")
    args = parser.parse_args()
    for run in sorted(path for path in args.runs.glob(args.pattern) if path.is_dir()):
        print(json.dumps(row(run, args.runs / "trace")))


if __name__ == "__main__":
    main()
