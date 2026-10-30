#!/usr/bin/env python3
"""Tabulate perf_run results of the protocol path scans and derive marginal per-viewer and per-source server cycles.

Runs are named <prefix>-<path>-<shape>; shapes b1/b2 differ only in viewers and c1/c2 in sources (each source keeps the same viewers per source)."""

import argparse
import gzip
import json
import re
from pathlib import Path


def load(path):
    gz = path / "result.json.gz"
    return json.load(gzip.open(gz)) if gz.exists() else json.loads((path / "result.json").read_text())


def row(path):
    r = load(path)
    stat = r.get("perf_stat", {})
    seconds = max(stat.get("seconds", 1), 1)
    busy = r.get("cpu_busy", {})
    server = r.get("process_metrics", {}).get("server", {})
    clients = {k: v for k, v in r.get("process_metrics", {}).items() if k not in ("server", "runner")}
    workers = sorted((v for v in server.get("thread_cpu_cores", {}).values() if v > 0.005), reverse=True)[:8]
    realtime = [float(g["measurement"]["viewer_realtime_min"]) for g in r["groups"] if "viewer_realtime_min" in g["measurement"]]
    probes = {k: v.get("ok") for k, v in (r.get("av_probe") or {}).items() if isinstance(v, dict)}
    probes.update({f"decode-{k}": v.get("ok") for k, v in (r.get("decode_probe") or {}).items()})
    cleanup = r.get("cleanup", {})
    control = sorted(sample["ms"] for sample in (r.get("control_probe") or {}).get("samples", []) if sample.get("error") is None)
    percentile = lambda q: control[min(len(control) - 1, int(q * len(control)))] if control else None
    measured = [g["measurement"] for g in r["groups"]]
    return {
        "run": path.name, "status": r["status"], "boundary": r.get("capacity_boundary"),
        "sources": sum((r["config"].get("input_counts") or {}).values()) or r["config"]["sources"],
        "viewers": sum(r["config"]["counts"].values()),
        "gcycles": stat.get("cycles", 0) / 1e9 / seconds, "ginstr": stat.get("instructions", 0) / 1e9 / seconds, "ipc": stat.get("ipc") or 0,
        "busy": busy.get("server_busy", 0), "max_core": busy.get("server_max_cpu_busy", 0), "softirq": busy.get("server_softirq", 0),
        "workers": workers, "worker_spread": (workers[0] - workers[-1]) if workers else 0,
        "client_cores": sum(v.get("cpu_cores", 0) for v in clients.values()), "client_max_core": busy.get("client_max_cpu_busy", 0),
        "pss_mib": server.get("pss_kib_max", 0) / 1024, "fd": server.get("fd_max", 0), "threads": len(server.get("thread_cpu_cores", {})),
        "udp_drops": r.get("server_evidence", {}).get("udp_drops"), "tcp_retrans": (r.get("namespace_tcp_delta") or {}).get("TcpRetransSegs"),
        "realtime_min": min(realtime) if realtime else None, "probes": probes,
        "control": (percentile(0.5), percentile(0.95), percentile(0.99)), "control_errors": (r.get("control_probe") or {}).get("errors"),
        "rss_mib": server.get("rss_kib_max", 0) / 1024,
        "out_mbps": sum((m.get("steady_media_bytes") or (m.get("steady_video_bytes") or 0) + (m.get("steady_audio_bytes") or 0)) * 8 / 1e6
                        for m in measured) / r["config"]["duration"],
        "lag_max": max((m["viewer_lag_span_ms_max"] for m in measured if m.get("viewer_lag_span_ms_max") is not None), default=None),
        "fd_after": (cleanup.get("server_settled") or {}).get("fd"), "fd_base": (r.get("baseline") or {}).get("fd"),
        "remaining": len(cleanup.get("remaining_owned_receivers") or []),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("runs", nargs="+", type=Path)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    rows = {}
    for path in args.runs:
        try:
            rows[path.name] = row(path)
        except (OSError, ValueError, KeyError) as error:
            rows[path.name] = {"run": path.name, "status": f"unreadable {error}"}
    if args.json:
        print(json.dumps(rows, indent=1))
        return
    print("| run | status | src | viewers | Gcycles/s | Ginstr/s | IPC | busy cores | busiest core | softirq | worker spread | client cores | PSS MiB | FD | UDP drops | realtime min | probes | FD base/after | left |")
    print("|" + "---|" * 19)
    for r in rows.values():
        if "gcycles" not in r:
            print(f"| {r['run']} | {r['status']} |" + " |" * 17)
            continue
        probes = " ".join(f"{k}:{'ok' if v else 'FAIL'}" for k, v in r["probes"].items()) or "—"
        rt = "—" if r["realtime_min"] is None else f"{r['realtime_min']:.3f}"
        print(f"| {r['run']} | {r['status']} | {r['sources']} | {r['viewers']} | {r['gcycles']:.2f} | {r['ginstr']:.2f} | {r['ipc']:.2f} | {r['busy']:.2f} | "
              f"{r['max_core']:.2f} | {r['softirq']:.3f} | {r['worker_spread']:.2f} | {r['client_cores']:.2f} | {r['pss_mib']:.0f} | {r['fd']} | {r['udp_drops']} | "
              f"{rt} | {probes} | {r['fd_base']}/{r['fd_after']} | {r['remaining']} |")
    print()
    print("| run | output Mbit/s (steady media received by clients) | lag span max ms | control p50/p95/p99 ms | control errors | TCP retrans | RSS MiB | threads |")
    print("|" + "---|" * 8)
    for r in rows.values():
        if "gcycles" not in r:
            continue
        fmt = lambda value, digits=1: "—" if value is None else f"{value:.{digits}f}"
        control = "/".join(fmt(v) for v in r["control"])
        print(f"| {r['run']} | {r['out_mbps']:.0f} | {fmt(r['lag_max'], 0)} | {control} | {r['control_errors']} | {r['tcp_retrans']} | {r['rss_mib']:.0f} | {r['threads']} |")
    print()
    print("| path | Mcycles/s per viewer (b2-b1) | Minstr/s per viewer | Mcycles/s per source incl. its viewers (c2-c1) | Mcycles/s per source excl. viewers |")
    print("|---|---|---|---|---|")
    paths = sorted({m.group(1) for name in rows if (m := re.match(r"(.+)-(b1|b2|c1|c2)$", name))})
    for path in paths:
        get = lambda shape: rows.get(f"{path}-{shape}", {})
        b1, b2, c1, c2 = get("b1"), get("b2"), get("c1"), get("c2")
        viewer = instr = per_source = excl = None
        if "gcycles" in b1 and "gcycles" in b2 and b2["viewers"] != b1["viewers"]:
            viewer = (b2["gcycles"] - b1["gcycles"]) * 1e3 / (b2["viewers"] - b1["viewers"])
            instr = (b2["ginstr"] - b1["ginstr"]) * 1e3 / (b2["viewers"] - b1["viewers"])
        if "gcycles" in c1 and "gcycles" in c2 and c2["sources"] != c1["sources"]:
            per_source = (c2["gcycles"] - c1["gcycles"]) * 1e3 / (c2["sources"] - c1["sources"])
            if viewer is not None:
                excl = per_source - viewer * (c2["viewers"] - c1["viewers"]) / (c2["sources"] - c1["sources"])
        fmt = lambda value: "—" if value is None else f"{value:.1f}"
        print(f"| {path} | {fmt(viewer)} | {fmt(instr)} | {fmt(per_source)} | {fmt(excl)} |")


if __name__ == "__main__":
    main()
