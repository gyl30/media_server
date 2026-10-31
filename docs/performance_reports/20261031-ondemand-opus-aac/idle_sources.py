#!/usr/bin/env python3
"""Measure server CPU for WHIP sources without any viewer. bench/perf_run.py cannot: with --outputs none it keeps one HTTP-FLV viewer per
source to confirm progress, which is an AAC consumer. Readiness is taken from /receivers instead."""

import argparse
import json
import subprocess
import sys
import time
from pathlib import Path


def pss_kib(pid):
    return sum(int(line.split()[1]) for line in Path(f"/proc/{pid}/smaps_rollup").read_text().splitlines() if line.startswith("Pss:"))


def udp_errors():
    lines = [line.split() for line in Path("/proc/net/snmp").read_text().splitlines() if line.startswith("Udp:")]
    fields = dict(zip(lines[0][1:], map(int, lines[1][1:])))
    return {key: fields[key] for key in ("InErrors", "RcvbufErrors")}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--server-bin", default="build/media_server")
    parser.add_argument("--sources", type=int, default=64)
    parser.add_argument("--workers", type=int, default=8)
    parser.add_argument("--warmup", type=int, default=15)
    parser.add_argument("--duration", type=int, default=60)
    parser.add_argument("--host", default="198.19.0.10")
    parser.add_argument("--client-address", default="198.19.0.11")
    parser.add_argument("--server-cpus", default="0-7")
    parser.add_argument("--client-cpus", default="8-15,24-31")
    args = parser.parse_args()
    runtime = args.runtime.resolve()
    sys.path.insert(0, str(runtime / "bench"))
    from signaling_stub import SignalingStub
    from perf_run import request
    args.output.mkdir(parents=True)
    env = {"LD_LIBRARY_PATH": str(runtime / "lib")}
    token = "idle-sources"
    processes = []
    with SignalingStub() as stub:
        server = subprocess.Popen(["taskset", "-c", args.server_cpus, str(runtime / args.server_bin), "--threads", str(args.workers), "--bind-address",
                                   args.host, "--webrtc-address", args.host, "--rtmp-port", "11935", "--rtsp-port", "18554", "--http-port", "18080",
                                   "--control-token", token, "--signaling-url", stub.url], stdout=(args.output / "server.log").open("w"),
                                  stderr=subprocess.STDOUT, env=env)
        processes.append(server)
        time.sleep(1)
        streams = [stub.issue("publish", ttl=600) for _ in range(args.sources)]
        (args.output / "publish.urls").write_text("".join(f"http://{args.host}:18080/publish/whip/{stream}\n" for stream in streams))
        processes.append(subprocess.Popen(["taskset", "-c", args.client_cpus, str(runtime / "build/whip_publisher"), "--whip-urls-file",
                                           str(args.output / "publish.urls"), "--fixture", str(runtime / ".cache/perf_fixture_whip.mkv"), "--sources",
                                           str(args.sources), "--bind-address", args.client_address, "--io-threads", "4", "--ramp-per-second", "200",
                                           "--duration", "86400"], stdout=(args.output / "publisher.log").open("w"), stderr=subprocess.STDOUT, env=env))
        deadline = time.monotonic() + 60
        ready = set()
        while time.monotonic() < deadline and len(ready) < args.sources:
            ready = {item["stream_id"] for item in request(f"http://{args.host}:18080", "/receivers", token=token)["receivers"]} & set(streams)
            time.sleep(0.5)
        result = {"server_bin": args.server_bin, "sources": args.sources, "ready": len(ready)}
        time.sleep(args.warmup)
        udp_before = udp_errors()
        stat = subprocess.run(["perf", "stat", "-x,", "-e", "cycles,instructions", "-p", str(server.pid), "--", "sleep", str(args.duration)],
                              capture_output=True, text=True)
        counters = {line.split(",")[2]: int(line.split(",")[0]) for line in stat.stderr.splitlines() if line and line.split(",")[0].isdigit()}
        udp_after = udp_errors()
        result.update({
            "gcycles_per_second": counters.get("cycles", 0) / 1e9 / args.duration,
            "ginstructions_per_second": counters.get("instructions", 0) / 1e9 / args.duration,
            "ipc": counters.get("instructions", 0) / max(1, counters.get("cycles", 0)),
            "pss_mib": pss_kib(server.pid) / 1024,
            "fd": len(list(Path(f"/proc/{server.pid}/fd").iterdir())),
            "threads": len(list(Path(f"/proc/{server.pid}/task").iterdir())),
            "udp_errors": {key: udp_after[key] - udp_before[key] for key in udp_before},
            "still_ready": len({item["stream_id"] for item in request(f"http://{args.host}:18080", "/receivers", token=token)["receivers"]} & set(streams)),
            "publisher_alive": processes[1].poll() is None,
        })
        for process in reversed(processes):
            process.kill()
            process.wait(timeout=10)
    log = (args.output / "server.log").read_text(errors="replace")
    result["log_errors"] = sum(" [error] " in line for line in log.splitlines())
    (args.output / "result.json").write_text(json.dumps(result, indent=1))
    print(json.dumps(result))


if __name__ == "__main__":
    main()
