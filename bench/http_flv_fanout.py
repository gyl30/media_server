#!/usr/bin/env python3

import argparse
import asyncio
import json
import os
import platform
import signal
import statistics
import subprocess
import time
from pathlib import Path


def proc_cpu(pid, tid=None):
    path = Path(f"/proc/{pid}/stat") if tid is None else Path(f"/proc/{pid}/task/{tid}/stat")
    stat = path.read_text().rsplit(") ", 1)[1].split()
    return (int(stat[11]) + int(stat[12])) / os.sysconf("SC_CLK_TCK")


def proc_status(pid, tid=None):
    path = Path(f"/proc/{pid}/status") if tid is None else Path(f"/proc/{pid}/task/{tid}/status")
    values = {}
    for line in path.read_text().splitlines():
        key, _, value = line.partition(":")
        if key in {"VmRSS", "voluntary_ctxt_switches", "nonvoluntary_ctxt_switches"}:
            values[key] = int(value.split()[0])
    return values


def proc_snapshot(pid):
    tasks = {}
    for task in Path(f"/proc/{pid}/task").iterdir():
        try:
            task_status = proc_status(pid, task.name)
            tasks[task.name] = {
                "cpu": proc_cpu(pid, task.name),
                "context_switches": task_status["voluntary_ctxt_switches"]
                + task_status["nonvoluntary_ctxt_switches"],
                "migrations": next(
                    int(line.split(":", 1)[1])
                    for line in Path(f"/proc/{pid}/task/{task.name}/sched").read_text().splitlines()
                    if line.startswith("se.nr_migrations")
                ),
            }
        except (FileNotFoundError, ProcessLookupError):
            continue

    pss_kib = 0
    for line in Path(f"/proc/{pid}/smaps_rollup").read_text().splitlines():
        if line.startswith("Pss:"):
            pss_kib = int(line.split()[1])
            break
    return {
        "cpu": proc_cpu(pid),
        "rss_kib": proc_status(pid)["VmRSS"],
        "pss_kib": pss_kib,
        "fd": len(list(Path(f"/proc/{pid}/fd").iterdir())),
        "threads": tasks,
    }


def loopback_packets():
    return int(Path("/sys/class/net/lo/statistics/rx_packets").read_text())


def percentile(values, fraction):
    ordered = sorted(values)
    position = (len(ordered) - 1) * fraction
    lower = int(position)
    upper = min(lower + 1, len(ordered) - 1)
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)


async def viewer(index, source_count, host, port, window, ready):
    received = 0
    error = None
    established = False
    writer = None
    try:
        reader, writer = await asyncio.open_connection(host, port)
        source = index % source_count
        request = f"GET /live/perf{source}.flv HTTP/1.1\r\nHost: {host}\r\nConnection: close\r\n\r\n"
        writer.write(request.encode())
        await writer.drain()
        header = await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), 10)
        if not header.startswith(b"HTTP/1.1 200"):
            raise RuntimeError(header.split(b"\r\n", 1)[0].decode(errors="replace"))
        first = await asyncio.wait_for(reader.read(1), 10)
        if not first:
            raise RuntimeError("media stream ended before first byte")
        established = True
        ready.set()
        while True:
            data = await asyncio.wait_for(reader.read(65536), 3)
            now = time.monotonic()
            if not data:
                raise RuntimeError("media stream ended")
            if window["start"] <= now < window["end"]:
                received += len(data)
            if now >= window["end"]:
                break
    except asyncio.CancelledError:
        pass
    except (OSError, RuntimeError, asyncio.TimeoutError, asyncio.IncompleteReadError) as caught:
        error = str(caught)
        ready.set()
    finally:
        if writer is not None:
            writer.close()
            try:
                await writer.wait_closed()
            except (OSError, asyncio.CancelledError):
                pass
    return {"bytes": received, "error": error, "established": established}


async def wait_for_listener(host, port):
    for _ in range(100):
        try:
            _, writer = await asyncio.open_connection(host, port)
            writer.close()
            await writer.wait_closed()
            return
        except OSError:
            await asyncio.sleep(0.1)
    raise RuntimeError("media server did not open RTMP listener")


async def wait_for_stream(host, port, source):
    for _ in range(100):
        try:
            reader, writer = await asyncio.open_connection(host, port)
            writer.write(f"GET /live/perf{source}.flv HTTP/1.1\r\nHost: {host}\r\n\r\n".encode())
            await writer.drain()
            header = await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), 2)
            writer.close()
            await writer.wait_closed()
            if header.startswith(b"HTTP/1.1 200"):
                return
        except (OSError, asyncio.TimeoutError, asyncio.IncompleteReadError):
            pass
        await asyncio.sleep(0.1)
    raise RuntimeError(f"source perf{source} did not become readable")


async def measure(args, server, publishers):
    await asyncio.gather(*(wait_for_stream(args.host, args.http_port, source) for source in range(args.sources)))
    window = {"start": float("inf"), "end": float("inf")}
    ready = [asyncio.Event() for _ in range(args.viewers)]
    viewers = []
    for index in range(args.viewers):
        viewers.append(asyncio.create_task(viewer(index, args.sources, args.host, args.http_port, window, ready[index])))
        if args.ramp_per_second and (index + 1) % args.ramp_per_second == 0:
            await asyncio.sleep(1)

    await asyncio.wait_for(asyncio.gather(*(event.wait() for event in ready)), 20)
    await asyncio.sleep(args.warmup)
    before_server = proc_snapshot(server.pid)
    before_publishers = [proc_cpu(publisher.pid) for publisher in publishers]
    before_client = proc_cpu(os.getpid())
    before_packets = loopback_packets()
    start = time.monotonic()
    window["start"] = start
    window["end"] = start + args.duration
    samples = []
    while time.monotonic() < window["end"]:
        await asyncio.sleep(min(1, max(0, window["end"] - time.monotonic())))
        samples.append(proc_snapshot(server.pid))
    elapsed = time.monotonic() - start
    after_packets = loopback_packets()
    after_client = proc_cpu(os.getpid())
    after_publishers = [proc_cpu(publisher.pid) for publisher in publishers]
    after_server = proc_snapshot(server.pid)
    for task in viewers:
        if not task.done():
            task.cancel()
    results = await asyncio.gather(*viewers)
    rates = [result["bytes"] / elapsed for result in results]
    thread_cpu = {
        tid: (state["cpu"] - before_server["threads"].get(tid, {"cpu": state["cpu"]})["cpu"]) / elapsed
        for tid, state in after_server["threads"].items()
    }
    thread_switches = {
        tid: state["context_switches"]
        - before_server["threads"].get(tid, {"context_switches": state["context_switches"]})["context_switches"]
        for tid, state in after_server["threads"].items()
    }
    thread_migrations = {
        tid: state["migrations"]
        - before_server["threads"].get(tid, {"migrations": state["migrations"]})["migrations"]
        for tid, state in after_server["threads"].items()
    }
    return {
        "elapsed_seconds": elapsed,
        "ready": sum(result["established"] for result in results),
        "progressing": sum(value > 0 for value in rates),
        "errors": [result["error"] for result in results if result["error"]],
        "aggregate_gbit_per_second": sum(rates) * 8 / 1e9,
        "viewer_bytes_per_second": {
            "min": min(rates),
            "p10": percentile(rates, 0.1),
            "p50": percentile(rates, 0.5),
            "p90": percentile(rates, 0.9),
            "max": max(rates),
        },
        "server_cpu_cores": (after_server["cpu"] - before_server["cpu"]) / elapsed,
        "client_cpu_cores": (after_client - before_client) / elapsed,
        "publisher_cpu_cores": sum(end - begin for begin, end in zip(before_publishers, after_publishers)) / elapsed,
        "server_rss_kib_median": statistics.median(sample["rss_kib"] for sample in samples),
        "server_pss_kib_median": statistics.median(sample["pss_kib"] for sample in samples),
        "server_fd_median": statistics.median(sample["fd"] for sample in samples),
        "server_thread_cpu_cores": thread_cpu,
        "server_thread_context_switches_per_second": {tid: value / elapsed for tid, value in thread_switches.items()},
        "server_thread_migrations_per_second": {tid: value / elapsed for tid, value in thread_migrations.items()},
        "loopback_packets_per_second": (after_packets - before_packets) / elapsed,
    }


def stop_process(process):
    if process.poll() is None:
        process.send_signal(signal.SIGTERM)
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


def main():
    parser = argparse.ArgumentParser(description="Direct RTMP publish to HTTP-FLV fanout benchmark")
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--server-bin", type=Path, default=Path("build/media_server"))
    parser.add_argument("--ffmpeg-bin", type=Path, default=Path("/home/gyl/bin/ffmpeg"))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--sources", type=int, default=1)
    parser.add_argument("--viewers", type=int, required=True)
    parser.add_argument("--workers", type=int, default=6)
    parser.add_argument("--ramp-per-second", type=int, default=100)
    parser.add_argument("--warmup", type=float, default=5)
    parser.add_argument("--duration", type=float, default=20)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--rtmp-port", type=int, default=11935)
    parser.add_argument("--rtsp-port", type=int, default=18554)
    parser.add_argument("--http-port", type=int, default=18080)
    args = parser.parse_args()
    if args.sources < 1 or args.viewers < 1 or args.workers < 1 or args.duration <= 0 or args.warmup < 0 or args.ramp_per_second < 1:
        parser.error("sources, viewers, workers, duration and ramp-per-second must be positive")
    args.output.mkdir(parents=True, exist_ok=True)
    server_log = (args.output / "server.log").open("w")
    server = subprocess.Popen(
        [str(args.server_bin), "--threads", str(args.workers), "--rtmp-port", str(args.rtmp_port), "--rtsp-port", str(args.rtsp_port),
         "--http-port", str(args.http_port), "--bind-address", args.host, "--webrtc-address", args.host],
        stdout=server_log, stderr=subprocess.STDOUT,
    )
    publishers = []
    publisher_logs = []
    try:
        asyncio.run(wait_for_listener(args.host, args.rtmp_port))
        for source in range(args.sources):
            publisher_log = (args.output / f"publisher-{source}.log").open("w")
            publisher_logs.append(publisher_log)
            publishers.append(subprocess.Popen(
                [str(args.ffmpeg_bin), "-hide_banner", "-loglevel", "error", "-stream_loop", "-1", "-re", "-i", str(args.fixture),
                 "-c", "copy", "-f", "flv", f"rtmp://{args.host}:{args.rtmp_port}/live/perf{source}"],
                stdout=publisher_log, stderr=subprocess.STDOUT,
            ))
        result = asyncio.run(measure(args, server, publishers))
        result["config"] = {
            "head": subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip(),
            "fixture": str(args.fixture), "sources": args.sources, "viewers": args.viewers, "workers": args.workers,
            "warmup_seconds": args.warmup, "duration_seconds": args.duration, "ramp_per_second": args.ramp_per_second,
            "kernel": platform.release(), "cpu_count": os.cpu_count(),
        }
        (args.output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
        print(json.dumps(result, indent=2))
        if result["ready"] != args.viewers or result["progressing"] != args.viewers or result["errors"]:
            raise SystemExit(1)
    finally:
        for publisher in publishers:
            stop_process(publisher)
        stop_process(server)
        for publisher_log in publisher_logs:
            publisher_log.close()
        server_log.close()


if __name__ == "__main__":
    main()
