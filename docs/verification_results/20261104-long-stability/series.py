#!/usr/bin/env python3
import gzip
import json
import statistics
import sys
from pathlib import Path


def load(path):
    path = Path(path)
    if path.is_dir():
        path = next(item for item in (path / "result.json", path / "result.json.gz", path / "run" / "result.json", path / "run" / "result.json.gz") if item.exists())
    opener = gzip.open if path.suffix == ".gz" else open
    with opener(path, "rt") as stream:
        return json.load(stream)


def slope_per_hour(points):
    if len(points) < 3:
        return None
    xs = [x for x, _ in points]
    ys = [y for _, y in points]
    mx = statistics.fmean(xs)
    my = statistics.fmean(ys)
    denominator = sum((x - mx) ** 2 for x in xs)
    return None if denominator == 0 else sum((x - mx) * (y - my) for x, y in points) / denominator * 3600


def server_series(result, buckets):
    samples = [sample["server"] for sample in result["samples"] if "server" in sample and "pss_kib" in sample["server"]]
    if len(samples) < 2:
        return {}
    start = samples[0]["monotonic_ns"]
    seconds = [(item["monotonic_ns"] - start) / 1e9 for item in samples]
    span = seconds[-1]
    rows = []
    for bucket in range(buckets):
        low, high = span * bucket / buckets, span * (bucket + 1) / buckets
        items = [item for item, at in zip(samples, seconds) if low <= at <= high]
        if len(items) < 2:
            continue
        elapsed = (items[-1]["monotonic_ns"] - items[0]["monotonic_ns"]) / 1e9
        rows.append({
            "t_s": round(high),
            "pss_mib": round(statistics.median(item["pss_kib"] for item in items) / 1024, 1),
            "rss_mib": round(statistics.median(item["rss_kib"] for item in items) / 1024, 1),
            "fd": round(statistics.median(item["fd"] for item in items)),
            "fd_max": max(item["fd"] for item in items),
            "cpu_cores": round((items[-1]["cpu"] - items[0]["cpu"]) / elapsed, 2),
            "tcp_tx_kib_max": round(max(item.get("tcp_tx_queue_bytes", 0) for item in items) / 1024),
            "udp_tx_kib_max": round(max(item.get("udp_tx_queue_bytes", 0) for item in items) / 1024),
            "udp_drops": items[-1].get("udp_drops", 0) - samples[0].get("udp_drops", 0),
        })
    half = [(at, item) for at, item in zip(seconds, samples) if at >= span / 2]
    quarter = max(1, len(samples) // 4)
    return {
        "samples": len(samples),
        "span_s": round(span),
        "pss_mib_slope_per_hour_second_half": round(slope_per_hour([(at, item["pss_kib"] / 1024) for at, item in half]) or 0, 2),
        "fd_slope_per_hour_second_half": round(slope_per_hour([(at, item["fd"]) for at, item in half]) or 0, 2),
        "pss_mib_first_quarter_median": round(statistics.median(item["pss_kib"] for item in samples[:quarter]) / 1024, 1),
        "pss_mib_last_quarter_median": round(statistics.median(item["pss_kib"] for item in samples[-quarter:]) / 1024, 1),
        "fd_first_quarter_median": statistics.median(item["fd"] for item in samples[:quarter]),
        "fd_last_quarter_median": statistics.median(item["fd"] for item in samples[-quarter:]),
        "buckets": rows,
    }


def group_rows(result):
    rows = []
    for group in result.get("groups", []):
        measurement = group.get("measurement", {})
        rows.append({
            "name": group.get("name"),
            "requested": group.get("requested"),
            "ready": measurement.get("ready"),
            "progressing": measurement.get("progressing"),
            "runtime_failures": measurement.get("runtime_failures"),
            "unprotect_failures": measurement.get("unprotect_failures"),
            "realtime_min": measurement.get("viewer_realtime_min"),
            "realtime_p10": measurement.get("viewer_realtime_p10"),
            "lag_span_ms_p90": measurement.get("viewer_lag_span_ms_p90"),
            "lag_span_ms_max": measurement.get("viewer_lag_span_ms_max"),
            "exit_code": group.get("exit_code"),
        })
    return rows


def source_churn_summary(value):
    rounds = value.get("rounds", [])
    return {
        "rounds": len(rounds),
        "failed": sum(not item.get("ok") for item in rounds),
        "lingering": sum(len(item.get("viewers_lingering", [])) for item in rounds),
        "stops": sorted({item.get("publisher_stop") for item in rounds}),
        "viewers_gone_ms_max": max((item.get("viewers_gone_ms") or 0 for item in rounds), default=None),
    }


def cleanup_summary(result):
    cleanup = result.get("cleanup") or {}
    after = cleanup.get("server_after") or {}
    settled = cleanup.get("server_settled") or {}
    baseline = result.get("baseline") or {}
    return {
        "remaining_receivers": cleanup.get("remaining_owned_receivers"),
        "reaped": cleanup.get("all_owned_processes_reaped"),
        "baseline_fd": baseline.get("fd"),
        "after_fd": after.get("fd"),
        "settled_fd": settled.get("fd"),
        "baseline_pss_mib": round(baseline.get("pss_kib", 0) / 1024, 1),
        "settled_pss_mib": round(settled.get("pss_kib", 0) / 1024, 1),
        "settled_threads": settled.get("thread_count"),
    }


def main():
    buckets = 10
    for path in sys.argv[1:]:
        result = load(path)
        server = result.get("process_metrics", {}).get("server", {})
        threads = sorted(server.get("thread_cpu_cores", {}).values(), reverse=True)
        workers = [round(value, 3) for value in threads if value > 0.001]
        print(json.dumps({
            "run": path,
            "status": result.get("status"),
            "boundary": result.get("capacity_boundary"),
            "errors": result.get("errors", [])[:10],
            "duration_s": result.get("config", {}).get("duration"),
            "server_cpu_cores": round(server.get("cpu_cores", 0), 3),
            "worker_cpu_cores": workers,
            "server_evidence": result.get("server_evidence"),
            "churn": {key: value for key, value in (result.get("churn") or {}).items() if not isinstance(value, list)},
            "source_churn": source_churn_summary(result.get("source_churn") or {}),
            "cleanup": cleanup_summary(result),
            "groups": group_rows(result),
            "series": server_series(result, buckets),
        }, ensure_ascii=False, indent=1))


if __name__ == "__main__":
    main()
