#!/usr/bin/env python3
"""Publish the fixed fixture using real signaling identities; optionally replace generations."""

import argparse
import json
import os
import signal
import subprocess
import time
from pathlib import Path
from urllib.parse import urlsplit

from fanout_support import stop_process
from perf_run import request, sha256


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--signaling-url", required=True)
    parser.add_argument("--input", choices=("rtmp", "rtsp_tcp", "rtsp_udp", "whip"), default="rtmp")
    parser.add_argument("--sources", type=int, default=1)
    parser.add_argument("--fixture", type=Path, default=Path(".cache/perf_fixture_720p30.flv"))
    parser.add_argument("--whip-fixture", type=Path, default=Path(".cache/perf_fixture_whip.mkv"))
    parser.add_argument("--ffmpeg-bin", type=Path, default=Path("bin/ffmpeg"))
    parser.add_argument("--client-dir", type=Path, default=Path("build"))
    parser.add_argument("--client-address", default="198.19.0.11")
    parser.add_argument("--client-cpus", default="8-15")
    parser.add_argument("--ramp-per-second", type=int, default=50)
    parser.add_argument("--group-size", type=int, default=256)
    parser.add_argument("--duration", type=int, default=60, help="seconds to hold each ready generation")
    parser.add_argument("--cycles", type=int, default=1, help="reissue publish identity for the same device IDs after stop")
    parser.add_argument("--ready-timeout", type=int, default=60)
    args = parser.parse_args()
    if min(args.sources, args.duration, args.cycles, args.ready_timeout, args.ramp_per_second, args.group_size) <= 0:
        parser.error("counts, timeouts and rate must be positive")
    args.output.mkdir(parents=True, exist_ok=False, mode=0o700)
    devices = []
    publishers = []
    logs = []
    result = {"status": "NOT_RUN", "input": args.input, "started_monotonic_ns": time.monotonic_ns(),
              "generations": [], "errors": [], "cleanup_errors": []}

    def interrupt(_number, _frame):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, interrupt)
    signal.signal(signal.SIGINT, interrupt)

    def stop_generation():
        for process in publishers:
            stop_process(process)
        publishers.clear()
        for device in devices:
            try:
                request(args.signaling_url, f"/api/push-devices/{device}/stop", {})
            except (OSError, RuntimeError) as error:
                result["cleanup_errors"].append(str(error))

    def launch(name, command):
        log = (args.output / f"{name}.log").open("w")
        logs.append(log)
        process = subprocess.Popen([str(item) for item in command], stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT)
        publishers.append(process)
        return process

    try:
        subprocess.run(["taskset", "-pc", args.client_cpus, str(os.getpid())], check=True,
                       capture_output=True, stdin=subprocess.DEVNULL)
        result["fixture_sha256"] = sha256(args.whip_fixture if args.input == "whip" else args.fixture)
        for index in range(args.sources):
            device = request(args.signaling_url, "/api/push-devices", {"name": f"perf-source-{index}"})
            devices.append(device["device_id"])
        for generation in range(args.cycles):
            sources = []
            urls = []
            for index, device in enumerate(devices):
                published = request(args.signaling_url, f"/api/push-devices/{device}/publish", {})
                sources.append({"stream_id": published["token"], "play_body": {"push_device_id": device}})
                if args.input == "whip":
                    urls.append(published["whip_url"])
                    if len(urls) == args.group_size or index + 1 == args.sources:
                        credentials = args.output / f"publish-{generation}-{index}.urls"
                        credentials.write_text("\n".join(urls) + "\n")
                        publisher = launch(f"publisher-{generation}-{index}", [args.client_dir / "whip_publisher", "--whip-urls-file", credentials,
                            "--fixture", args.whip_fixture, "--sources", len(urls), "--bind-address", args.client_address,
                            "--io-threads", min(4, len(urls)), "--ramp-per-second", args.ramp_per_second, "--duration", 86400])
                        for source in sources[-len(urls):]:
                            source["publisher_pid"] = publisher.pid
                        urls.clear()
                else:
                    command = [args.ffmpeg_bin, "-nostdin", "-hide_banner", "-loglevel", "error", "-threads", 1, "-re", "-stream_loop", -1,
                               "-i", args.fixture, "-c", "copy"]
                    if args.input == "rtmp":
                        command += ["-f", "flv", published["rtmp_url"]]
                    else:
                        command += ["-f", "rtsp", "-rtsp_transport", "udp" if args.input == "rtsp_udp" else "tcp", published["rtsp_url"]]
                    sources[-1]["publisher_pid"] = launch(f"publisher-{generation}-{index}", command).pid
                    time.sleep(1 / args.ramp_per_second)
            deadline = time.monotonic() + args.ready_timeout
            for offset in range(0, args.sources, args.group_size):
                selected = sources[offset:offset + args.group_size]
                attempt = 0
                while True:
                    credentials = args.output / f"probe-{generation}-{offset}-{attempt}.paths"
                    try:
                        answers = [request(args.signaling_url, "/api/play", {"source": source["play_body"]}) for source in selected]
                    except RuntimeError as error:
                        if "HTTP 409" not in str(error) or time.monotonic() >= deadline:
                            raise
                        time.sleep(0.1)
                        continue
                    if any(answer["stream_id"] != source["stream_id"] for source, answer in zip(selected, answers)):
                        raise RuntimeError("source generation changed during readiness")
                    credentials.write_text("\n".join(urlsplit(answer["flv_url"]).path.lstrip("/") for answer in answers) + "\n")
                    host = urlsplit(answers[0]["flv_url"])
                    command = [args.client_dir / "http_flv_fanout", "--play-paths-file", credentials, "--media-host", host.hostname,
                               "--media-port", host.port or 80, "--viewers", len(selected), "--ready-timeout", 5,
                               "--warmup", 0, "--duration", 1, "--ramp-per-second", args.ramp_per_second]
                    log = (args.output / f"probe-{generation}-{offset}-{attempt}.log").open("w")
                    logs.append(log)
                    probe = subprocess.Popen([str(item) for item in command], stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT)
                    try:
                        code = probe.wait(timeout=8)
                    finally:
                        stop_process(probe)
                    if code == 0:
                        break
                    if time.monotonic() >= deadline:
                        raise TimeoutError("source group did not produce both tracks")
                    attempt += 1
                    time.sleep(0.1)
            temporary = args.output / "sources.tmp"
            temporary.write_text(json.dumps(sources, indent=2) + "\n")
            temporary.replace(args.output / "sources.json")
            result["generations"].append({"generation": generation, "sources": sources, "ready_monotonic_ns": time.monotonic_ns(),
                                           "publisher_pids": [process.pid for process in publishers]})
            print(f"phase=media_ready generation={generation} sources={len(sources)} sources_file={args.output / 'sources.json'}", flush=True)
            started = time.monotonic()
            while time.monotonic() - started < args.duration:
                if any(process.poll() is not None for process in publishers):
                    raise RuntimeError("publisher exited while holding the generation")
                time.sleep(0.1)
            stop_generation()
        result["status"] = "PASS"
    except KeyboardInterrupt:
        result["status"] = "PASS" if result["generations"] else "FAIL"
        result["stopped_by_operator"] = True
    except (OSError, ValueError, RuntimeError, TimeoutError, subprocess.SubprocessError) as error:
        result["status"] = "FAIL"
        result["errors"].append({"message": str(error), "monotonic_ns": time.monotonic_ns()})
    finally:
        stop_generation()
        for device in devices:
            try:
                request(args.signaling_url, f"/api/push-devices/{device}", method="DELETE")
            except (OSError, RuntimeError) as error:
                result["cleanup_errors"].append(str(error))
        for log in logs:
            log.close()
        if result["cleanup_errors"]:
            result["status"] = "FAIL"
        result["ended_monotonic_ns"] = time.monotonic_ns()
        result["cleanup"] = {"all_owned_publishers_reaped": not publishers, "devices_deleted": not result["cleanup_errors"]}
        (args.output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({"status": result["status"], "result": str(args.output / "result.json")}))
    return 0 if result["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
