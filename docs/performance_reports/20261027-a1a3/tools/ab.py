#!/usr/bin/env python3
import json
import re
import sys
from pathlib import Path

import costs


def phase(line):
    return {key: value for key, value in re.findall(r"(\w+)=(\S+)", line)}


def lines(path, prefix):
    if not path.exists():
        return []
    return [phase(line) for line in path.read_text(errors="replace").splitlines() if line.startswith(prefix)]


def worst(items, key):
    values = [float(item[key]) for item in items if key in item]
    return max(values) if values else None


def total(items, key):
    values = [float(item[key]) for item in items if key in item]
    return sum(values) if values else None


def row(path):
    result = json.loads((path / "result.json").read_text())
    base = costs.row(path)
    exe = result.get("server_identity", {}).get("exe", "")
    variant = re.search(r"/(rt-[^/]+)/build", exe)
    groups = [path / f"{group['name']}.log" for group in result.get("groups", [])]
    churn = sorted(path.glob("churn-*.log"))
    status, boundary = base["status"], base["boundary"]
    if status == "PASS" and base["realtime_min"] is not None and base["realtime_min"] < 0.98:
        status, boundary = "FAIL", "SERVER_SATURATED" if (base["server_busy"] or 0) >= 7.2 else "UNCONFIRMED"
    out = {"run": path.name, "variant": variant.group(1) if variant else exe, "status": status, "boundary": boundary,
           "outputs": base["outputs"], "server_busy": base["server_busy"], "mhz": base["server_busy_mhz"], "mcyc_viewer": base["mcycles_per_viewer"],
           "minstr_viewer": base["minstr_per_viewer"], "ipc": base["ipc"], "pss_mib": base["pss_mib"], "fd_max": base["fd_max"],
           "rt_min": base["realtime_min"], "lag_p50": base["lag_span_p50"], "lag_max": base["lag_span_max"], "ctx_s": base["ctx_per_s"]}
    measured = []
    for group in result.get("groups", []):
        item = group["measurement"]
        if item.get("duration_seconds"):
            measured.append((item.get("steady_media_bytes", 0) / item["duration_seconds"], item.get("viewer_bytes_per_second_min", 0)))
        elif item.get("measurement_started") and item.get("measurement_end_monotonic_ns"):
            seconds = (item["measurement_end_monotonic_ns"] - item["measurement_start_monotonic_ns"]) / 1e9
            measured.append(((item["steady_video_bytes"] + item["steady_audio_bytes"]) / seconds,
                             (item["viewer_video_bytes_min"] + item["viewer_audio_bytes_min"]) / seconds))
    if measured:
        out["gbit_s"] = sum(rate for rate, _ in measured) * 8 / 1e9
        out["viewer_kbit_min"] = min(rate for _, rate in measured) * 8 / 1e3
    probe = result.get("control_probe") or {}
    out.update({f"probe_{key}": probe.get(key) for key in ("requests", "errors", "over_1s", "ms_p50", "ms_p90", "ms_p99", "ms_p100")})
    rounds = (result.get("churn") or {}).get("rounds", [])
    out["churn_ok"] = f"{sum(1 for item in rounds if item.get('ok'))}/{len(rounds)}" if rounds else None
    for name, logs in (("main", groups), ("churn", churn)):
        setup = [item for log in logs for item in lines(log, "phase=setup ")]
        established = [item for log in logs for item in lines(log, "phase=established ")]
        control = [item for log in logs for item in lines(log, "phase=control ")]
        cleanup = [item for log in logs for item in lines(log, "phase=cleanup ")]
        if setup:
            out.update({f"{name}_post_p99": worst(setup, "post_ms_p99"), f"{name}_post_max": worst(setup, "post_ms_max"),
                        f"{name}_est_p50": worst(setup, "establish_ms_p50"), f"{name}_est_p99": worst(setup, "establish_ms_p99"),
                        f"{name}_est_max": worst(setup, "establish_ms_max"), f"{name}_est_fail": total(established, "establishment_failures"),
                        f"{name}_ramp_s_max": worst(established, "ramp_seconds")})
        if control:
            out.update({f"{name}_create_p50": worst(control, "create_ms_p50"), f"{name}_create_p99": worst(control, "create_ms_p99"),
                        f"{name}_create_max": worst(control, "create_ms_max"), f"{name}_delete_p50": worst(control, "delete_ms_p50"),
                        f"{name}_delete_p99": worst(control, "delete_ms_p99"), f"{name}_delete_max": worst(control, "delete_ms_max"),
                        f"{name}_deleted": f"{int(total(cleanup, 'deleted') or 0)}/{int(total(cleanup, 'created') or 0)}"})
    evidence = result.get("server_evidence", {})
    out["udp_drops"] = evidence.get("udp_drops")
    out["retrans"] = (result.get("namespace_tcp_delta") or {}).get("RetransSegs")
    after = result.get("cleanup", {}).get("server_after") or {}
    out["after_fd_thr_pss"] = f"{after.get('fd')}/{after.get('thread_count')}/{after.get('pss_kib', 0) / 1024:.0f}" if after else None
    return out


def main():
    rows = [row(Path(item)) for item in sys.argv[1:] if (Path(item) / "result.json").exists()]
    keys = []
    for item in rows:
        keys += [key for key in item if key not in keys and any(other.get(key) is not None for other in rows)]
    print("\t".join(keys))
    for item in rows:
        print("\t".join(f"{item.get(key):.4g}" if isinstance(item.get(key), float) else str(item.get(key, "")) for key in keys))


if __name__ == "__main__":
    main()
