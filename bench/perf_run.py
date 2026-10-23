#!/usr/bin/env python3
"""Authenticated media workload with one readiness barrier and bounded cleanup."""

import argparse
import contextlib
import hashlib
import http.client
import json
import math
import os
import re
import resource
import secrets
import shutil
import signal
import struct
import subprocess
import sys
import time
import uuid
from pathlib import Path
from urllib.parse import urlsplit

from fanout_support import benchmark_head, parse_phase, proc_identity, proc_snapshot, stop_process, wait_for_listener


MSR_MPERF = 0xE7
MSR_APERF = 0xE8
MSR_TSC = 0x10
OUTPUTS = ("rtmp", "rtsp", "flv", "hls", "whep", "gb_udp", "gb_tcp_active", "gb_tcp_passive")
INPUTS = ("rtmp", "rtsp_tcp", "rtsp_udp", "rtsp_pull", "rtmp_pull", "whep_pull", "whip", "gb_udp", "gb_tcp_active", "gb_tcp_passive", "existing")


def request(base, path, body=None, token="", method=None):
    parsed = urlsplit(base)
    connection = http.client.HTTPConnection(parsed.hostname, parsed.port or 80, timeout=15)
    try:
        headers = {"Authorization": f"Bearer {token}"} if token else {}
        payload = None
        if body is not None:
            headers["Content-Type"] = "application/json"
            payload = json.dumps(body)
        connection.request(method or ("POST" if body is not None else "GET"), parsed.path.rstrip("/") + path, payload, headers)
        response = connection.getresponse()
        data = response.read()
        if response.status >= 400:
            raise RuntimeError(f"HTTP {response.status} {path}: {data.decode(errors='replace')}")
        return json.loads(data) if data else {}
    finally:
        connection.close()


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def redacted_command(command):
    redacted = []
    redact_next = False
    for value in command:
        if redact_next:
            redacted.append("<redacted>")
            redact_next = False
        elif value == "--control-token":
            redacted.append(value)
            redact_next = True
        elif value.startswith("--control-token="):
            redacted.append("--control-token=<redacted>")
        else:
            redacted.append(re.sub(r"(?<![0-9a-f])[0-9a-f]{64}(?![0-9a-f])", "<token>", value))
    return redacted


def cpu_set(text):
    cpus = set()
    for part in text.split(","):
        first, _, last = part.partition("-")
        cpus.update(range(int(first), int(last or first) + 1))
    return cpus


def cpu_times():
    times = {}
    for line in Path("/proc/stat").read_text().splitlines():
        name, *values = line.split()
        if name.startswith("cpu") and name != "cpu":
            times[int(name[3:])] = [int(value) for value in values]
    return times


def cpu_counters(cpus):
    counters = []
    for cpu in sorted(cpus):
        try:
            descriptor = os.open(f"/dev/cpu/{cpu}/msr", os.O_RDONLY)
        except OSError:
            return None
        try:
            counters.append([struct.unpack("<Q", os.pread(descriptor, 8, register))[0] for register in (MSR_MPERF, MSR_APERF, MSR_TSC)])
        finally:
            os.close(descriptor)
    return {"monotonic_ns": time.monotonic_ns(), "cpus": counters}


def cpu_frequency(before, after):
    if not before or not after:
        return None
    mperf, aperf, tsc = (sum(late[index] - early[index] for early, late in zip(before["cpus"], after["cpus"])) for index in range(3))
    seconds = (after["monotonic_ns"] - before["monotonic_ns"]) / 1e9
    if not mperf or not tsc or seconds <= 0:
        return None
    tsc_mhz = tsc / len(before["cpus"]) / seconds / 1e6
    return {"busy_mhz": tsc_mhz * aperf / mperf, "c0_residency": mperf / tsc, "tsc_mhz": tsc_mhz, "gcycles_per_second": aperf / seconds / 1e9}


def cpu_usage(before, after, cpus):
    ticks = os.sysconf("SC_CLK_TCK")
    usage = {"busy": 0.0, "user": 0.0, "system": 0.0, "irq": 0.0, "softirq": 0.0, "max_cpu_busy": 0.0}
    for cpu in cpus:
        delta = [late - early for early, late in zip(before[cpu], after[cpu])]
        total = sum(delta[:8])
        if total <= 0:
            continue
        busy = total - delta[3] - delta[4]
        usage["max_cpu_busy"] = max(usage["max_cpu_busy"], busy / total)
        elapsed = total / ticks
        for key, value in (("busy", busy), ("user", delta[0] + delta[1]), ("system", delta[2]), ("irq", delta[5]), ("softirq", delta[6])):
            usage[key] += value / ticks / elapsed
    return usage


def workload_counts(text):
    counts = {}
    if text == "none":
        return counts
    for entry in text.split(","):
        protocol, count = entry.split(":")
        if protocol not in OUTPUTS or int(count) <= 0 or protocol in counts:
            raise ValueError("outputs must be distinct protocol:positive_count entries")
        counts[protocol] = int(count)
    return counts


def parse_arguments():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True, help="new run directory, never overwritten")
    parser.add_argument("--input", choices=INPUTS, default="rtmp")
    parser.add_argument("--outputs", default="rtmp:1", help="rtmp:10,rtsp:10,flv:10,hls:10,whep:10,gb_udp:10 or none (PARTIAL publish-only: continuous FLV viewer per source)")
    parser.add_argument("--sources", type=int, default=1)
    parser.add_argument("--sources-file", type=Path, help="existing sources JSON: [{stream_id, play_body:{push_device_id|source_id|live_id:...}}]")
    parser.add_argument("--signaling-url", help="real signaling /api/play issuer; required for existing input")
    parser.add_argument("--server-log", type=Path, help="existing DUT log, read only for queue-full evidence")
    parser.add_argument("--server-pid", type=int, help="existing DUT, sampled only; never stopped by this tool")
    parser.add_argument("--server-bin", type=Path, default=Path("build/media_server"))
    parser.add_argument("--client-dir", type=Path, default=Path("build"))
    parser.add_argument("--ffmpeg-bin", type=Path, default=Path("bin/ffmpeg"))
    parser.add_argument("--ffprobe-bin", type=Path, default=Path("bin/ffprobe"))
    parser.add_argument("--fixture", type=Path, default=Path(".cache/perf_fixture_720p30.flv"))
    parser.add_argument("--whip-fixture", type=Path, default=Path(".cache/perf_fixture_whip.mkv"))
    parser.add_argument("--host", default="198.19.0.10")
    parser.add_argument("--client-address", default="198.19.0.11")
    parser.add_argument("--rtmp-port", type=int, default=11935)
    parser.add_argument("--rtsp-port", type=int, default=18554)
    parser.add_argument("--http-port", type=int, default=18080)
    parser.add_argument("--gb-port-base", type=int, default=22000)
    parser.add_argument("--workers", type=int, default=8)
    parser.add_argument("--server-cpus", default="0-7")
    parser.add_argument("--client-cpus", default="8-15")
    parser.add_argument("--control-token", default="", help="Bearer for existing DUT; self-managed run generates one")
    parser.add_argument("--duration", type=int, default=30)
    parser.add_argument("--warmup", type=int, default=5)
    parser.add_argument("--ready-timeout", type=int, default=60)
    parser.add_argument("--progress-interval", type=int, default=5)
    parser.add_argument("--ramp-per-second", type=int, default=50)
    parser.add_argument("--group-size", type=int, default=256, help="max viewers per client process; independent credentials per group")
    parser.add_argument("--min-free-memory-mib", type=int, default=0, help="operator-defined safety floor; 0 records without enforcing")
    parser.add_argument("--min-free-disk-mib", type=int, default=0)
    parser.add_argument("--max-server-pss-mib", type=int, default=0)
    parser.add_argument("--decode-probe", action="store_true", help="decode six seconds of each RTMP/RTSP/FLV/HLS output with ffprobe during the window")
    parser.add_argument("--churn", help="PROTOCOL:COUNT extra viewers that repeatedly join and leave during the window")
    parser.add_argument("--perf-stat", action="store_true", help="count server CPU cycles, instructions, page faults and context switches with perf stat during the window")
    parser.add_argument("--cleanup-settle", type=int, default=0, help="seconds to wait after cleanup before a second server snapshot")
    parser.add_argument("--churn-period", type=int, default=20, help="seconds per churn round; viewers stay for period minus 8 seconds")
    parser.add_argument("--min-realtime-ratio", type=float, default=0.98, help="per-viewer video media time advanced over the measurement window divided by wall time")
    parser.add_argument("--fault", choices=("none", "invalid_token", "expired_token", "replayed_token", "source_stop"), default="none", help="bounded negative validation, not a capacity boundary")
    args = parser.parse_args()
    try:
        args.counts = workload_counts(args.outputs)
    except ValueError as error:
        parser.error(str(error))
    if min(args.sources, args.duration, args.workers, args.ready_timeout, args.progress_interval, args.ramp_per_second, args.group_size) <= 0 or args.warmup < 0:
        parser.error("counts, duration, timeouts and rate must be positive; warmup nonnegative")
    if not 0 < args.min_realtime_ratio <= 1:
        parser.error("min-realtime-ratio must be in (0, 1]")
    if args.churn:
        protocol, _, count = args.churn.partition(":")
        if protocol not in OUTPUTS or protocol == "hls" or not count.isdigit() or int(count) <= 0 or args.churn_period < 12:
            parser.error("churn requires PROTOCOL:COUNT for a native client and a period of at least 12 seconds")
        args.churn = (protocol, int(count))
    if min(args.min_free_memory_mib, args.min_free_disk_mib, args.max_server_pss_mib) < 0:
        parser.error("resource budgets must be nonnegative")
    if "hls" in args.counts and args.duration < 6:
        parser.error("HLS duration must cover its 6-second progress interval")
    if args.duration < args.progress_interval:
        parser.error("duration must cover a progress interval")
    if args.input == "existing" and not (args.sources_file and args.signaling_url and args.server_pid and args.control_token):
        parser.error("existing input requires sources-file, signaling-url, server-pid and control-token")
    if args.input == "existing" and args.fault != "none":
        parser.error("fault injection owns fixture sources/tokens; existing mode never alters external resources")
    if args.input != "existing" and (args.server_pid or args.signaling_url or args.sources_file):
        parser.error("self-managed fixtures and externally owned sources must be separate")
    if args.gb_port_base + sum(args.counts.values()) + args.sources >= 65536:
        parser.error("GB passive port range exceeds 65535")
    gb_tcp = [protocol for protocol in (args.input, *args.counts, args.churn[0] if args.churn else "") if protocol.startswith("gb_tcp")]
    gb_port_end = args.gb_port_base + args.sources + sum(args.counts.values()) + (args.churn[1] if args.churn else 0)
    ephemeral_low, ephemeral_high = map(int, Path("/proc/sys/net/ipv4/ip_local_port_range").read_text().split())
    if gb_tcp and args.gb_port_base <= ephemeral_high and gb_port_end > ephemeral_low:
        parser.error(f"GB TCP listen ports {args.gb_port_base}-{gb_port_end - 1} overlap ephemeral ports {ephemeral_low}-{ephemeral_high}; lower --gb-port-base")
    service_ports = sorted(port + offset for port in (args.rtmp_port, args.rtsp_port, args.http_port) for offset in (0, 100))
    if gb_tcp and any(args.gb_port_base <= port < gb_port_end for port in service_ports):
        parser.error(f"GB TCP listen ports {args.gb_port_base}-{gb_port_end - 1} overlap service ports {service_ports}; move --gb-port-base")
    return args


def run(args):
    args.output.mkdir(parents=True, exist_ok=False, mode=0o700)
    result = {"status": "NOT_RUN", "head": benchmark_head(), "config": {key: "<redacted>" if key == "control_token" and value else str(value) if isinstance(value, Path) else value for key, value in vars(args).items()},
              "auth_mode": "real_signaling" if args.input == "existing" else "one_time_verifier_fixture", "errors": [], "groups": [], "samples": [], "cleanup": {}}
    def interrupt(_number, _frame):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, interrupt)
    signal.signal(signal.SIGINT, interrupt)
    processes = []
    publishers = []
    clients = []
    source_ids = []
    upstream_pairs = []
    gb_senders = []
    logs = []
    server = upstream = None
    process_startticks = {}
    process_identities = {}
    external_processes = []
    next_budget_check = 0
    control_token = args.control_token or secrets.token_hex(32)
    control = f"http://{args.host}:{args.http_port}"
    script_dir = Path(__file__).resolve().parent
    barrier = args.output / "measurement.start"
    preparation_started = time.monotonic()
    timeout = args.ready_timeout + math.ceil(sum(args.counts.values()) / args.ramp_per_second)
    server_cpus = cpu_set(args.server_cpus)
    client_cpus = cpu_set(args.client_cpus)

    def check_identity(name, pid, identity=None):
        try:
            identity = identity if identity is not None else proc_identity(pid)
        except (FileNotFoundError, ProcessLookupError):
            category = next((category for _, process, _, category in processes if process.pid == pid), None)
            if category == "client" or (category == "publisher" and result.get("fault_evidence", {}).get("completed_monotonic_ns")):
                raise
            result["status"] = "INCONCLUSIVE" if name == "server" and category is None else "FAIL"
            result["capacity_boundary"] = "UPSTREAM_FAILED" if category == "publisher" or "publisher" in name else "SERVER_UNSTABLE" if name == "server" and category == "server" else "UNCONFIRMED"
            raise RuntimeError(f"{name} PID {pid} exited before sampling")
        if ((pid in process_startticks and identity["starttick"] != process_startticks[pid]) or
                (pid in process_identities and identity != process_identities[pid])):
            result["status"] = "BLOCKED"
            result["capacity_boundary"] = "PROCESS_IDENTITY"
            raise RuntimeError(f"{name} PID {pid} identity changed; sampling another process is forbidden")
        return identity

    def check_budget(force=False, phase="preparation", server_pid=None, server_pss_kib=None):
        nonlocal next_budget_check
        now = time.monotonic()
        if not force and now < next_budget_check:
            return
        next_budget_check = now + 1
        for name, pid in external_processes:
            check_identity(name, pid)
        for name, process, _, category in processes:
            if category in ("server", "publisher", "client") and process.poll() is None:
                try:
                    check_identity(name, process.pid)
                except (FileNotFoundError, ProcessLookupError) as error:
                    try:
                        process.wait(timeout=1)
                    except subprocess.TimeoutExpired:
                        raise error from None
        if not (args.min_free_memory_mib or args.min_free_disk_mib or args.max_server_pss_mib):
            return
        memory = next(int(line.split()[1]) for line in Path("/proc/meminfo").read_text().splitlines() if line.startswith("MemAvailable:"))
        disk = shutil.disk_usage(args.output).free
        pid = server_pid or (server.pid if server else args.server_pid)
        pss = None
        if args.max_server_pss_mib and server_pss_kib is not None:
            pss = server_pss_kib
        elif args.max_server_pss_mib and pid is not None:
            check_identity("server", pid)
            pss = next(int(line.split()[1]) for line in Path(f"/proc/{pid}/smaps_rollup").read_text().splitlines() if line.startswith("Pss:"))
        sample = {"phase": phase, "monotonic_ns": time.monotonic_ns(), "memory_available_kib": memory,
                  "disk_available_bytes": disk, "server_pss_kib": pss}
        result.setdefault("budget_samples", []).append(sample)
        if (memory < args.min_free_memory_mib * 1024 or disk < args.min_free_disk_mib * 1024 * 1024 or
                (pss is not None and pss > args.max_server_pss_mib * 1024)):
            result["status"] = "BLOCKED"
            result["capacity_boundary"] = "RESOURCE_BUDGET"
            raise RuntimeError("operator-defined resource budget reached before or during measurement")

    def launch(name, command, cpus, category):
        check_budget(force=True)
        command = [str(item) for item in command]
        if shutil.which(command[0]) is None:
            raise FileNotFoundError(f"CLIENT_STARTUP: executable missing: {command[0]}")
        log = (args.output / f"{name}.log").open("w")
        logs.append(log)
        process = subprocess.Popen(["taskset", "-c", cpus, *command], stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT)
        processes.append((name, process, command, category))
        process_startticks[process.pid] = proc_identity(process.pid)["starttick"]
        check_budget(force=True, server_pid=process.pid if name == "server" else None)
        return process

    def client_command(protocol, group_name, selected, gb_listen_port, warmup, duration, ready_timeout, start_file):
        credentials = args.output / f"{group_name}.credentials"
        common = ["--viewers", len(selected), "--warmup", warmup, "--duration", duration,
                  "--ramp-per-second", args.ramp_per_second, "--ready-timeout", ready_timeout, "--start-file", start_file]
        if protocol.startswith("gb_"):
            token_file = args.output / "control.token"
            if not token_file.exists():
                token_file.write_text(control_token + "\n")
                token_file.chmod(0o600)
            senders = [{"stream_id": item["stream_id"], "sender_id": str(uuid.uuid4())} for item in selected]
            gb_senders.extend(senders)
            credentials.write_text("\n".join(f"{item['stream_id']}/{item['sender_id']}" for item in senders) + "\n")
            return [args.client_dir / "gb_fanout", "--play-paths-file", credentials, "--media-host", args.host,
                    "--media-port", args.http_port, "--control-token-file", token_file, "--transport", protocol[3:],
                    "--bind-address", args.client_address, "--listen-port", gb_listen_port,
                    "--progress-interval", args.progress_interval, *common]
        urls = [play_value(source, protocol, stub) for source in selected]
        if protocol in ("rtmp", "rtsp", "flv"):
            credentials.write_text("\n".join(urlsplit(url).path.lstrip("/") for url in urls) + "\n")
            tool = {"flv": "http_flv"}.get(protocol, protocol)
            return [args.client_dir / f"{tool}_fanout", "--play-paths-file", credentials, "--media-host", args.host,
                    "--media-port", {"rtmp": args.rtmp_port, "rtsp": args.rtsp_port, "flv": args.http_port}[protocol],
                    "--progress-interval", args.progress_interval, *common]
        credentials.write_text("\n".join(urls) + "\n")
        if protocol == "hls":
            return [sys.executable, script_dir / "hls_client.py", "--hls-urls-file", credentials,
                    "--persistent", "--progress-interval", 6, "--output", args.output / f"{group_name}.json", *common]
        return [args.client_dir / "whep_fanout", "--whep-urls-file", credentials,
                "--bind-address", args.client_address, "--io-threads", 1, "--progress-interval", args.progress_interval, *common]

    def receiver_list(base):
        return {entry["stream_id"] for entry in request(base, "/receivers", token=control_token)["receivers"]}

    def media_server(name, host, ports, verifier):
        process = launch(name, [args.server_bin, "--threads", args.workers, "--bind-address", host, "--webrtc-address", host,
                               "--rtmp-port", ports[0], "--rtsp-port", ports[1], "--http-port", ports[2],
                               "--control-token", control_token, "--signaling-url", verifier], args.server_cpus if name == "server" else args.client_cpus, "server" if name == "server" else "publisher")
        wait_for_listener(host, ports[2], check=lambda: check_budget(server_pid=process.pid if name == "server" else None))
        process_identities[process.pid] = check_identity(name, process.pid)
        return process

    def ffmpeg_source(name, source, transport, host, ports):
        command = [args.ffmpeg_bin, "-nostdin", "-hide_banner", "-loglevel", "error", "-threads", "1", "-re", "-stream_loop", "-1", "-i", args.fixture, "-c", "copy"]
        if transport == "rtmp":
            command += ["-f", "flv", f"rtmp://{host}:{ports[0]}/live/{source}"]
        else:
            command += ["-f", "rtsp", "-rtsp_transport", "udp" if transport == "rtsp_udp" else "tcp", f"rtsp://{host}:{ports[1]}/{source}"]
        process = launch(name, command, args.client_cpus, "publisher")
        publishers.append(process)
        return process

    def play_value(source, protocol, stub, host=None, ports=None):
        host = host or args.host
        ports = ports or (args.rtmp_port, args.rtsp_port, args.http_port)
        if stub is None:
            answer = request(args.signaling_url, "/api/play", {"source": source["play_body"]})
            if answer["stream_id"] != source["stream_id"]:
                raise RuntimeError("source generation changed; obtain a new sources-file before measuring")
            return answer[{"rtmp": "rtmp_url", "rtsp": "rtsp_url", "flv": "flv_url", "hls": "hls_url", "whep": "whep_url"}[protocol]]
        token = stub.issue("play", source["stream_id"], ttl=args.ready_timeout + 60)
        if args.fault == "invalid_token":
            token = secrets.token_hex(32)
        elif args.fault == "expired_token":
            token = stub.issue("play", source["stream_id"], ttl=0)
        elif args.fault == "replayed_token":
            request(stub.url, "/internal/verify", {"operation": "play", "stream_id": source["stream_id"], "token": token})
        path = f"{source['stream_id']}/{token}"
        if protocol == "rtmp":
            return f"rtmp://{host}:{ports[0]}/{path}"
        if protocol == "rtsp":
            return f"rtsp://{host}:{ports[1]}/{path}"
        if protocol == "flv":
            return f"http://{host}:{ports[2]}/{path}.flv"
        return f"http://{host}:{ports[2]}/play/{protocol}/{path}" + ("/index.m3u8" if protocol == "hls" else "")

    def probe(source, name, stub, host=None, ports=None):
        source_group = source if isinstance(source, list) else [source]
        old_fault = args.fault
        args.fault = "none"
        try:
            deadline = time.monotonic() + args.ready_timeout
            attempt = 0
            while time.monotonic() < deadline:
                check_budget()
                path = args.output / f"{name}-{attempt}.credentials"
                path.write_text("\n".join(urlsplit(play_value(item, "flv", stub, host, ports)).path.lstrip("/") for item in source_group) + "\n")
                command = [args.client_dir / "http_flv_fanout", "--play-paths-file", path, "--media-host", host or args.host,
                           "--media-port", (ports or (args.rtmp_port, args.rtsp_port, args.http_port))[2], "--viewers", len(source_group), "--ramp-per-second", args.ramp_per_second,
                           "--warmup", 0, "--duration", 1, "--ready-timeout", 5]
                process = launch(f"{name}-{attempt}", command, args.client_cpus, "probe")
                probe_deadline = time.monotonic() + 8
                while process.poll() is None:
                    check_budget()
                    remaining = probe_deadline - time.monotonic()
                    if remaining <= 0:
                        raise subprocess.TimeoutExpired(command, 8)
                    try:
                        process.wait(timeout=min(1, remaining))
                    except subprocess.TimeoutExpired:
                        pass
                if process.returncode == 0:
                    return
                attempt += 1
                time.sleep(0.1)
            raise TimeoutError(f"source group {name} did not produce both tracks")
        finally:
            args.fault = old_fault

    owned = args.input != "existing"
    stub = None
    try:
        if not owned:
            identity = proc_identity(args.server_pid)
            if not Path(f"/proc/{args.server_pid}/exe").samefile(args.server_bin.resolve()):
                result["status"] = "BLOCKED"
                result["capacity_boundary"] = "PROCESS_IDENTITY"
                raise RuntimeError("existing server-pid executable does not match --server-bin")
            process_identities[args.server_pid] = identity
            external_processes.append(("server", args.server_pid))
        check_budget(force=True)
        subprocess.run(["taskset", "-pc", args.client_cpus, str(os.getpid())], check=True, capture_output=True)
        if owned:
            from signaling_stub import SignalingStub
            auth_context = SignalingStub()
        else:
            auth_context = contextlib.nullcontext(None)
        with auth_context as stub:
            if owned:
                result["fixture_sha256"] = sha256(args.fixture)
                metadata = subprocess.run([str(args.ffprobe_bin), "-v", "error", "-show_format", "-show_streams", "-of", "json", str(args.fixture)],
                                          capture_output=True, text=True, check=True, timeout=30)
                result["fixture_metadata"] = json.loads(metadata.stdout)
                codecs = {track["codec_type"]: track["codec_name"] for track in result["fixture_metadata"]["streams"]}
                if codecs.get("video") not in ("h264", "hevc") or codecs.get("audio") not in ("aac", "opus", "pcm_alaw", "pcm_mulaw") or len(codecs) != 2:
                    result["status"] = "UNSUPPORTED"
                    raise ValueError("fixture must contain one H.264/H.265 video track and one AAC/Opus/G.711 audio track")
                server = media_server("server", args.host, (args.rtmp_port, args.rtsp_port, args.http_port), stub.url)
                server_pid = server.pid
                if args.input in ("rtsp_pull", "rtmp_pull", "whep_pull", "gb_udp", "gb_tcp_active", "gb_tcp_passive"):
                    upstream_ports = (args.rtmp_port + 100, args.rtsp_port + 100, args.http_port + 100)
                    upstream_control = f"http://{args.client_address}:{upstream_ports[2]}"
                    upstream = media_server("upstream", args.client_address, upstream_ports, stub.url)
                sources = []
                whip_ids = []
                if args.input == "whip":
                    result["whip_fixture_sha256"] = sha256(args.whip_fixture)
                    for offset in range(0, args.sources, args.group_size):
                        ids = [stub.issue("publish", ttl=60) for _ in range(min(args.group_size, args.sources - offset))]
                        whip_ids.extend(ids)
                        urls = args.output / f"publish-{offset}.urls"
                        urls.write_text("\n".join(f"http://{args.host}:{args.http_port}/publish/whip/{stream}" for stream in ids) + "\n")
                        publisher = launch(f"publisher-{offset}", [args.client_dir / "whip_publisher", "--whip-urls-file", urls,
                            "--fixture", args.whip_fixture, "--sources", len(ids), "--bind-address", args.client_address,
                            "--io-threads", min(4, len(ids)), "--ramp-per-second", args.ramp_per_second, "--duration", 86400], args.client_cpus, "publisher")
                        publishers.append(publisher)
                for index in range(args.sources):
                    if args.input in ("rtmp", "rtsp_tcp", "rtsp_udp", "whip"):
                        stream = whip_ids[index] if args.input == "whip" else stub.issue("publish", ttl=60)
                        source = {"stream_id": stream}
                        if args.input != "whip":
                            ffmpeg_source(f"publisher-{index}", stream, args.input, args.host, (args.rtmp_port, args.rtsp_port, args.http_port))
                    else:
                        upstream_stream = stub.issue("publish", ttl=60)
                        ffmpeg_source(f"publisher-{index}", upstream_stream, "rtmp", args.client_address, upstream_ports)
                        probe({"stream_id": upstream_stream}, f"upstream-ready-{index}", stub, args.client_address, upstream_ports)
                        stream = str(uuid.uuid4())
                        source = {"stream_id": stream}
                        if args.input.endswith("_pull"):
                            url = play_value({"stream_id": upstream_stream}, args.input[:-5], stub, args.client_address, upstream_ports)
                            request(control, "/pull/create", {"stream_id": stream, "url": url}, control_token)
                        else:
                            ssrc = 100000000 + index
                            sender = {"stream_id": upstream_stream, "sender_id": str(uuid.uuid4()), "payload_type": 96, "ssrc": ssrc}
                            receiver = {"stream_id": stream, "payload_type": 96, "ssrc": ssrc}
                            port = args.gb_port_base + index
                            if args.input == "gb_udp":
                                receiver["transport"] = "udp"
                                ports = request(control, "/gb28181/receiver/create", receiver, control_token)
                                sender.update(transport="udp", remote_address=args.host, remote_rtp_port=ports["rtp_port"])
                                request(upstream_control, "/gb28181/sender/create", sender, control_token)
                            elif args.input == "gb_tcp_passive":
                                receiver.update(transport="tcp_passive", listen_port=port)
                                sender.update(transport="tcp_active", remote_address=args.host, remote_port=port)
                                request(control, "/gb28181/receiver/create", receiver, control_token)
                                request(upstream_control, "/gb28181/sender/create", sender, control_token)
                            else:
                                sender.update(transport="tcp_passive", listen_port=port)
                                receiver.update(transport="tcp_active", remote_address=args.client_address, remote_port=port)
                                request(upstream_control, "/gb28181/sender/create", sender, control_token)
                                request(control, "/gb28181/receiver/create", receiver, control_token)
                            upstream_pairs.append((upstream_control, sender))
                    source_ids.append(stream)
                    sources.append(source)
                for offset in range(0, len(sources), args.group_size):
                    probe(sources[offset:offset + args.group_size], f"source-ready-{offset}", stub)
            else:
                sources = json.loads(args.sources_file.read_text())
                if len(sources) != args.sources or len({item["stream_id"] for item in sources}) != args.sources:
                    raise ValueError("sources-file must contain exactly the distinct requested sources")
                source_ids = [source["stream_id"] for source in sources]
                server_pid = args.server_pid
                for source in sources:
                    if "publisher_pid" not in source:
                        continue
                    pid = source["publisher_pid"]
                    if "publisher_starttick" not in source:
                        raise ValueError("sources-file publisher_pid requires publisher_starttick; regenerate it with perf_publish")
                    process_startticks[pid] = source["publisher_starttick"]
                    process_identities[pid] = check_identity(f"external-publisher-{pid}", pid)
                    if (f"external-publisher-{pid}", pid) not in external_processes:
                        external_processes.append((f"external-publisher-{pid}", pid))
                for offset in range(0, len(sources), args.group_size):
                    probe(sources[offset:offset + args.group_size], f"source-ready-{offset}", None)
            result["source_ids"] = source_ids
            result["baseline"] = proc_snapshot(server_pid)
            process_identities[server_pid] = check_identity("server", server_pid)
            result["server_identity"] = process_identities[server_pid]
            if set(result["baseline"]["cpu_affinity"]) & os.sched_getaffinity(0):
                raise ValueError("server and client CPU affinity overlap")
            counts = args.counts or {"flv": args.sources}
            preparation_sample = {"cpu": cpu_times(), "monotonic_ns": time.monotonic_ns()}
            result["preparation_cpu"] = {"server_busy_max": 0.0, "server_max_cpu_busy": 0.0, "client_busy_max": 0.0, "client_max_cpu_busy": 0.0}

            def sample_preparation():
                if time.monotonic_ns() - preparation_sample["monotonic_ns"] < 1000000000:
                    return
                current = cpu_times()
                for prefix, cpus in (("server", server_cpus), ("client", client_cpus)):
                    usage = cpu_usage(preparation_sample["cpu"], current, cpus)
                    result["preparation_cpu"][f"{prefix}_busy_max"] = max(result["preparation_cpu"][f"{prefix}_busy_max"], usage["busy"])
                    result["preparation_cpu"][f"{prefix}_max_cpu_busy"] = max(result["preparation_cpu"][f"{prefix}_max_cpu_busy"], usage["max_cpu_busy"])
                preparation_sample.update(cpu=current, monotonic_ns=time.monotonic_ns())

            next_client_start = time.monotonic()
            for protocol, count in counts.items():
                for offset in range(0, count, args.group_size):
                    size = min(args.group_size, count - offset)
                    while time.monotonic() < next_client_start:
                        check_budget()
                        sample_preparation()
                        time.sleep(min(1, max(0, next_client_start - time.monotonic())))
                    group_name = f"{protocol}-{offset}"
                    selected = [sources[index % len(sources)] for index in range(offset, offset + size)]
                    command = client_command(protocol, group_name, selected, args.gb_port_base + args.sources + offset,
                                             args.warmup, args.duration, timeout, barrier)
                    process = launch(group_name, command, args.client_cpus, "client")
                    clients.append((group_name, process, size))
                    next_client_start = time.monotonic() + size / args.ramp_per_second
            deadline = time.monotonic() + timeout
            while True:
                check_budget()
                sample_preparation()
                if any(process.poll() is not None for _, process, _ in clients):
                    raise RuntimeError("client exited before all media_ready; inspect group logs")
                if all("phase=media_ready " in (args.output / f"{name}.log").read_text() for name, _, _ in clients):
                    break
                if time.monotonic() > deadline:
                    raise TimeoutError("all-viewer readiness timed out")
                time.sleep(0.05)
            for name, process, _, category in processes:
                if category in ("server", "publisher", "client") and process.poll() is None:
                    process_identities[process.pid] = check_identity(name, process.pid)
            result["process_identities"] = {str(pid): identity for pid, identity in process_identities.items()}
            result["preparation_seconds"] = time.monotonic() - preparation_started
            start_ns = time.monotonic_ns() + (args.warmup + 1) * 1000000000
            temporary = barrier.with_suffix(".tmp")
            temporary.write_text(str(start_ns) + "\n")
            temporary.replace(barrier)
            deadline_ns = start_ns + args.duration * 1000000000
            result["requested_window"] = {"start_monotonic_ns": start_ns, "end_monotonic_ns": deadline_ns}
            while time.monotonic_ns() < start_ns:
                check_budget(phase="warmup")
                sample_preparation()
                time.sleep(min(1, max(0, (start_ns - time.monotonic_ns()) / 1e9)))
            decode_probes = []
            if args.decode_probe:
                for protocol in [protocol for protocol in counts if protocol in ("rtmp", "rtsp", "flv", "hls")]:
                    command = [args.ffprobe_bin, "-v", "error", "-read_intervals", "%+6", "-count_frames", "-show_entries",
                               "stream=codec_type,codec_name,profile,width,height,sample_rate,channels,nb_read_frames", "-of", "json"]
                    command += ["-rtsp_transport", "tcp"] if protocol == "rtsp" else []
                    command.append(play_value(sources[0], protocol, stub))
                    decode_probes.append((protocol, launch(f"decode-{protocol}", command, args.client_cpus, "decode")))
            server_log_path = args.output / "server.log" if owned else args.server_log
            server_log_offset = server_log_path.stat().st_size if server_log_path else 0
            monitored = [("server", server_pid), ("runner", os.getpid())] + [(name, process.pid) for name, process, _, category in processes if category in ("publisher", "client")]
            external_publishers = sorted({source["publisher_pid"] for source in sources if "publisher_pid" in source})
            monitored += [(f"external-publisher-{pid}", pid) for pid in external_publishers]
            next_sample = start_ns
            stopped_source = False
            churn_process = None
            churn_next_ns = start_ns
            churn_rounds = []

            def finish_churn(process):
                round_result = churn_rounds[-1]
                lines = (args.output / f"{round_result['name']}.log").read_text().splitlines()
                measurements = [parse_phase(line) for line in lines if line.startswith("phase=measurement ")]
                measurement = measurements[-1] if measurements else {}
                round_result.update(exit_code=process.returncode, ready=measurement.get("ready"), progressing=measurement.get("progressing"),
                                    finished_monotonic_ns=time.monotonic_ns())
                round_result["ok"] = (process.returncode == 0 and measurement.get("ready") == round_result["requested"] and
                                      measurement.get("progressing") == round_result["requested"])

            sibling_cpus = set()
            for cpu in server_cpus:
                with contextlib.suppress(OSError, ValueError):
                    sibling_cpus |= cpu_set(Path(f"/sys/devices/system/cpu/cpu{cpu}/topology/thread_siblings_list").read_text().strip())
            sibling_cpus -= server_cpus
            cpu_start = previous_cpu = cpu_times()
            previous_cpu_ns = time.monotonic_ns()
            server_interval_busy_max = 0.0
            counters_start = {"server": cpu_counters(server_cpus), "client": cpu_counters(client_cpus)}
            perf_stat = None
            if args.perf_stat:
                perf_stat = subprocess.Popen(["perf", "stat", "-a", "-C", ",".join(map(str, sorted(server_cpus))), "-x", "\t", "-e",
                                              "cycles,instructions,ref-cycles,page-faults,context-switches", "-o", args.output / "perf-stat.txt", "--", "sleep", str(args.duration)],
                                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            while time.monotonic_ns() < deadline_ns:
                snapshots = {}
                netns_queues = {}
                socket_round = len(result["samples"]) % 5 == 0
                for name, pid in monitored:
                    try:
                        check_identity(name, pid)
                        snapshots[name] = proc_snapshot(pid, netns_queues, sockets=socket_round)
                        check_identity(name, pid, snapshots[name]["identity"])
                    except (FileNotFoundError, ProcessLookupError):
                        snapshots[name] = {"exited": True, "monotonic_ns": time.monotonic_ns()}
                result["samples"].append(snapshots)
                memory = next(int(line.split()[1]) for line in Path("/proc/meminfo").read_text().splitlines() if line.startswith("MemAvailable:"))
                system = {"monotonic_ns": time.monotonic_ns(), "memory_available_kib": memory,
                          "disk_available_bytes": shutil.disk_usage(args.output).free,
                          "psi": {kind: Path(f"/proc/pressure/{kind}").read_text() for kind in ("cpu", "memory", "io")}}
                if time.monotonic_ns() - previous_cpu_ns >= 500000000:
                    current_cpu = cpu_times()
                    server_interval_busy_max = max(server_interval_busy_max, cpu_usage(previous_cpu, current_cpu, server_cpus)["busy"])
                    previous_cpu, previous_cpu_ns = current_cpu, time.monotonic_ns()
                result.setdefault("system_samples", []).append(system)
                check_budget(force=True, phase="measurement", server_pss_kib=snapshots.get("server", {}).get("pss_kib"))
                if args.churn:
                    if churn_process is not None and churn_process.poll() is not None:
                        finish_churn(churn_process)
                        churn_process = None
                    now_ns = time.monotonic_ns()
                    if churn_process is None and now_ns >= churn_next_ns and now_ns + args.churn_period * 1000000000 <= deadline_ns:
                        protocol, count = args.churn
                        name = f"churn-{len(churn_rounds)}"
                        churn_start = args.output / f"{name}.start"
                        churn_start.write_text(str(now_ns + (math.ceil(count / args.ramp_per_second) + 3) * 1000000000) + "\n")
                        command = client_command(protocol, name, [sources[index % len(sources)] for index in range(count)],
                                                 args.gb_port_base + args.sources + sum(counts.values()), 0, args.churn_period - 8,
                                                 math.ceil(count / args.ramp_per_second) + 10, churn_start)
                        churn_rounds.append({"name": name, "requested": count, "launched_monotonic_ns": now_ns,
                                             "server_fd_before": snapshots.get("server", {}).get("fd")})
                        churn_process = launch(name, command, args.client_cpus, "churn")
                        churn_next_ns = now_ns + args.churn_period * 1000000000
                if args.fault == "source_stop" and not stopped_source and time.monotonic_ns() >= start_ns + args.duration * 500000000:
                    result["fault_evidence"] = {"kind": "source_stop", "requested_monotonic_ns": time.monotonic_ns(), "media_ready_groups": len(clients)}
                    for process in publishers:
                        if process.poll() is not None:
                            raise RuntimeError("publisher exited before source_stop injection")
                        process.kill()
                    for process in publishers:
                        process.wait(timeout=5)
                    result["fault_evidence"]["completed_monotonic_ns"] = time.monotonic_ns()
                    result["fault_evidence"]["publishers"] = [{"pid": process.pid, "exit_code": process.poll()} for process in publishers]
                    stopped_source = True
                next_sample += 1000000000
                time.sleep(max(0, (min(next_sample, deadline_ns) - time.monotonic_ns()) / 1e9))
            if churn_process is not None:
                try:
                    churn_process.wait(timeout=args.churn_period)
                except subprocess.TimeoutExpired:
                    stop_process(churn_process)
                finish_churn(churn_process)
            if args.churn:
                result["churn"] = {"protocol": args.churn[0], "viewers_per_round": args.churn[1], "period_seconds": args.churn_period,
                                   "rounds": churn_rounds, "failed_rounds": sum(not item["ok"] for item in churn_rounds)}
            final = {}
            netns_queues = {}
            for name, pid in monitored:
                try:
                    check_identity(name, pid)
                    final[name] = proc_snapshot(pid, netns_queues)
                    check_identity(name, pid, final[name]["identity"])
                except (FileNotFoundError, ProcessLookupError):
                    final[name] = {"exited": True, "monotonic_ns": time.monotonic_ns()}
            result["samples"].append(final)
            if "namespace_snmp" in final.get("server", {}):
                result["namespace_tcp_delta"] = {key: value - result["baseline"]["namespace_snmp"].get(key, value)
                                                 for key, value in final["server"]["namespace_snmp"].items()}
            for protocol, process in decode_probes:
                try:
                    process.wait(timeout=60)
                except subprocess.TimeoutExpired:
                    stop_process(process)
                output = (args.output / f"decode-{protocol}.log").read_text()
                try:
                    streams = json.loads(output[output.index("{"):]).get("streams", [])
                except ValueError:
                    streams = []
                frames = {stream.get("codec_type"): int(stream.get("nb_read_frames") or 0) for stream in streams}
                result.setdefault("decode_probe", {})[protocol] = {"exit_code": process.returncode, "streams": streams,
                    "ok": process.returncode == 0 and frames.get("video", 0) > 0 and all(frames.values())}
            if perf_stat is not None:
                perf_stat.wait(timeout=30)
                perf_counts = {}
                for line in (args.output / "perf-stat.txt").read_text().splitlines():
                    fields = line.split("\t")
                    if len(fields) > 2 and fields[0].replace(".", "", 1).isdigit():
                        perf_counts[fields[2]] = int(float(fields[0]))
                result["perf_stat"] = {**perf_counts, "seconds": args.duration,
                                       "ipc": perf_counts["instructions"] / perf_counts["cycles"] if perf_counts.get("cycles") else None}
            cpu_end = cpu_times()
            frequency = {prefix: cpu_frequency(counters_start[prefix], cpu_counters(cpus)) or {} for prefix, cpus in (("server", server_cpus), ("client", client_cpus))}
            result["cpu_busy"] = {
                "server_cpus": sorted(server_cpus), "server_sibling_cpus": sorted(sibling_cpus), "client_cpus": sorted(client_cpus),
                **{f"server_{key}": value for key, value in cpu_usage(cpu_start, cpu_end, server_cpus).items()},
                "server_interval_busy_max": server_interval_busy_max,
                **{f"sibling_{key}": value for key, value in cpu_usage(cpu_start, cpu_end, sibling_cpus).items()},
                **{f"client_{key}": value for key, value in cpu_usage(cpu_start, cpu_end, client_cpus).items()},
                **{f"{prefix}_{key}": value for prefix, values in frequency.items() for key, value in values.items()}}
            if server_log_path:
                with server_log_path.open() as log:
                    log.seek(server_log_offset)
                    text = log.read()
                result["server_evidence"] = {"queue_full": text.count("udp write queue full"),
                                             "no_buffer_space": text.count("No buffer space available")}
            for name, process, count in clients:
                process.wait(timeout=timeout)
                lines = (args.output / f"{name}.log").read_text().splitlines()
                measurements = [parse_phase(line) for line in lines if line.startswith("phase=measurement ")]
                media = measurements[-1] if measurements else {}
                resources = [parse_phase(line) for line in lines if line.startswith("phase=client_resources ")]
                if resources:
                    media.update(resources[-1])
                latency = [parse_phase(line) for line in lines if line.startswith("first_media_ms_p50=")]
                result["groups"].append({"name": name, "requested": count, "exit_code": process.returncode,
                                         "measurement": media, "first_media": latency[-1] if latency else {}, "log": f"{name}.log"})
            for group in result["groups"]:
                media = group["measurement"]
                actual_start = media.get("measurement_start_monotonic_ns", 0)
                actual_end = media.get("measurement_end_monotonic_ns", 0)
                overlap = max(0, min(actual_end, deadline_ns) - max(actual_start, start_ns)) if actual_start > 0 and actual_end > actual_start else 0
                group["window"] = {"actual_start_monotonic_ns": actual_start, "actual_end_monotonic_ns": actual_end,
                                   "actual_seconds": max(0, actual_end - actual_start) / 1e9,
                                   "start_delay_seconds": (actual_start - start_ns) / 1e9 if actual_start else None,
                                   "end_delay_seconds": (actual_end - deadline_ns) / 1e9 if actual_end else None,
                                   "coverage": overlap / (args.duration * 1e9), "required_coverage": 0.99}
            success = all(item["exit_code"] == 0 and item["measurement"].get("ready") == item["requested"] and
                          item["measurement"].get("progressing") == item["requested"] and item["window"]["coverage"] >= 0.99
                          for item in result["groups"])
            success = success and all(probe["ok"] for probe in result.get("decode_probe", {}).values())
            success = success and not result.get("churn", {}).get("failed_rounds")
            result["status"] = "PASS" if success else "FAIL"
            result["capacity_boundary"] = "HEALTHY" if success else "UNCONFIRMED"
            if server is not None and server.poll() is not None:
                result["status"] = "FAIL"
                result["capacity_boundary"] = "SERVER_UNSTABLE"
            if any(process.poll() is not None for process in publishers) and args.fault == "none":
                result["status"] = "FAIL"
                result["capacity_boundary"] = "UPSTREAM_FAILED"
            server_samples = [sample["server"] for sample in result["samples"] if "udp_drops" in sample["server"]]
            if len(server_samples) >= 2:
                result.setdefault("server_evidence", {})["udp_drops"] = server_samples[-1]["udp_drops"] - server_samples[0]["udp_drops"]
            if any(value > 0 for value in result.get("server_evidence", {}).values()):
                result["status"] = "FAIL"
                result["capacity_boundary"] = "SERVER_UNSTABLE"
            if result.get("server_evidence", {}).get("udp_drops", 0) < 0:
                result["server_evidence"]["udp_drops_interpretation"] = "socket aggregate decreased; historical drops are not recoverable"
                if result["capacity_boundary"] == "HEALTHY":
                    result["status"] = "INCONCLUSIVE"
                    result["capacity_boundary"] = "UNCONFIRMED"
            group_by_name = {group["name"]: group for group in result["groups"]}
            for name, pid in monitored:
                samples = [sample[name] for sample in result["samples"] if name in sample and not sample[name].get("exited")]
                if len(samples) < 2:
                    continue
                begin, end = samples[0], samples[-1]
                socket_samples = [item for item in samples if "udp_drops" in item]
                udp_drops = socket_samples[-1]["udp_drops"] - socket_samples[0]["udp_drops"] if socket_samples else 0
                elapsed = (end["monotonic_ns"] - begin["monotonic_ns"]) / 1e9
                group_media = group_by_name.get(name, {}).get("measurement", {})
                cores = group_media.get("client_cpu_cores", (end["cpu"] - begin["cpu"]) / elapsed)
                result.setdefault("process_metrics", {})[name] = {"cpu_cores": cores, "elapsed_seconds": elapsed,
                    "rss_kib_max": max(item["rss_kib"] for item in samples), "pss_kib_max": max(item["pss_kib"] for item in samples),
                    "fd_max": max(item["fd"] for item in samples), "affinity": end["cpu_affinity"], "cpu_method": "client_window_counter" if "client_cpu_cores" in group_media else "proc_window_delta",
                    "thread_cpu_cores": {tid: (state["cpu"] - begin["threads"].get(tid, state)["cpu"]) / elapsed for tid, state in end["threads"].items()},
                    "thread_sample_window": {"start": begin["monotonic_ns"], "end": end["monotonic_ns"]}}
                hot_thread = max(result["process_metrics"][name]["thread_cpu_cores"].values(), default=0) >= 0.9
                if name not in ("server", "runner") and (hot_thread or udp_drops > 0 or cores >= 0.9 * min(1 if name.startswith(tuple(OUTPUTS)) else len(end["cpu_affinity"]), len(end["cpu_affinity"])) or
                                         end["fd"] >= resource.getrlimit(resource.RLIMIT_NOFILE)[0] * 0.8):
                    result["status"] = "BLOCKED" if not success else "INCONCLUSIVE"
                    result["capacity_boundary"] = "CLIENT_RESOURCE"
            if result["status"] == "PASS" and len(result["samples"]) < 0.8 * args.duration:
                result["status"] = "INCONCLUSIVE"
                result["capacity_boundary"] = "RUNNER_RESOURCE"
            lagging = [group["name"] for group in result["groups"] if group["measurement"].get("measurement_started") and
                       group["measurement"].get("viewer_realtime_min", args.min_realtime_ratio) < args.min_realtime_ratio]
            if lagging:
                result["realtime_lagging_groups"] = lagging
            busy = result.get("cpu_busy", {})
            server_hot_thread = max(result.get("process_metrics", {}).get("server", {}).get("thread_cpu_cores", {}).values(), default=0)
            saturated = (busy.get("server_busy", 0) >= 0.9 * len(server_cpus) or busy.get("server_max_cpu_busy", 0) >= 0.98 or
                         server_hot_thread >= 0.95)
            if saturated and (lagging or result["status"] == "FAIL") and result["capacity_boundary"] in ("HEALTHY", "UNCONFIRMED"):
                result["status"] = "FAIL"
                result["capacity_boundary"] = "SERVER_SATURATED"
            elif lagging and result["status"] == "PASS":
                result["status"] = "FAIL"
                result["capacity_boundary"] = "UNCONFIRMED"
            client_cores = sum(value["cpu_cores"] for name, value in result.get("process_metrics", {}).items() if name != "server")
            if client_cores >= 0.9 * len(os.sched_getaffinity(0)):
                result["status"] = "BLOCKED" if result["status"] != "PASS" else "INCONCLUSIVE"
                result["capacity_boundary"] = "CLIENT_RESOURCE"
            if stub is not None:
                result["verification_calls"] = stub.count()
    except (OSError, subprocess.SubprocessError, TimeoutError, RuntimeError, ValueError, KeyboardInterrupt) as error:
        if result["status"] not in ("UNSUPPORTED", "BLOCKED", "INCONCLUSIVE"):
            result["status"] = "BLOCKED" if isinstance(error, OSError) and not isinstance(error, TimeoutError) else "FAIL"
        result.setdefault("capacity_boundary", "UNCONFIRMED")
        result["errors"].append({"type": type(error).__name__, "message": str(error), "monotonic_ns": time.monotonic_ns()})
    finally:
        if result["status"] == "NOT_RUN":
            result["status"] = "FAIL"
            error = sys.exc_info()[1]
            if error is not None:
                result["errors"].append({"type": type(error).__name__, "message": str(error), "monotonic_ns": time.monotonic_ns()})
        result["server_exit_before_cleanup"] = server.poll() if server else None
        for name, process, count in clients:
            if not any(group["name"] == name for group in result["groups"]):
                lines = (args.output / f"{name}.log").read_text().splitlines()
                measurements = [parse_phase(line) for line in lines if line.startswith("phase=measurement ")]
                result["groups"].append({"name": name, "requested": count, "exit_code": process.poll(), "measurement": measurements[-1] if measurements else {}, "log": f"{name}.log"})
            group = next(group for group in result["groups"] if group["name"] == name)
            text = (args.output / f"{name}.log").read_text()
            group["first_errors"] = [line for line in text.splitlines() if "error=" in line or line.startswith("trigger_")][:20]
            if process.poll() not in (None, 0) or group["measurement"].get("failed", 0):
                if "Too many open files" in text or "bad_alloc" in text:
                    category = "CLIENT_RESOURCE"
                elif args.fault in ("invalid_token", "expired_token", "replayed_token") or "authorization" in text or "HTTP 403" in text:
                    category = "AUTHORIZATION"
                elif "first_media_timeout" in text:
                    category = "FIRST_MEDIA_TIMEOUT"
                elif result.get("fault_evidence", {}).get("kind") == "source_stop" or "source_ended" in text:
                    category = "SOURCE_ENDED"
                elif "non_progressing" in text or "phase=progress progressing=0" in text:
                    category = "MEDIA_STALL"
                elif process.poll() in (2, 126, 127):
                    category = "CLIENT_STARTUP"
                else:
                    category = "CONNECTION_OR_PROTOCOL"
                group["failure_category"] = category
        if stub is not None:
            result["verification"] = stub.summary()
        for _, process, _ in clients:
            stop_process(process)
        if owned and server is not None and server.poll() is None:
            deletions = ([(control, "/receivers/delete", {"stream_id": stream}) for stream in source_ids] +
                         [(control, "/gb28181/sender/delete", sender) for sender in gb_senders] +
                         [(base, "/gb28181/sender/delete", {"stream_id": sender["stream_id"], "sender_id": sender["sender_id"]})
                          for base, sender in upstream_pairs])
            for base, path, body in deletions:
                try:
                    request(base, path, body, control_token)
                except RuntimeError as error:
                    if "HTTP 404" not in str(error):
                        result["cleanup"].setdefault("errors", []).append(str(error))
                except OSError as error:
                    result["cleanup"].setdefault("errors", []).append(f"{path}: {error}; remaining deletions skipped")
                    break
            for process in publishers:
                stop_process(process)
            time.sleep(0.5)
            try:
                result["cleanup"]["remaining_owned_receivers"] = sorted(receiver_list(control) & set(source_ids))
                result["cleanup"]["server_after"] = proc_snapshot(server.pid)
                if args.cleanup_settle:
                    time.sleep(args.cleanup_settle)
                    result["cleanup"]["server_settled"] = proc_snapshot(server.pid)
            except (OSError, RuntimeError) as error:
                result["cleanup"].setdefault("errors", []).append(str(error))
        for _, process, _, _ in reversed(processes):
            stop_process(process)
        for log in logs:
            log.close()
        result["cleanup"]["all_owned_processes_reaped"] = all(process.poll() is not None for _, process, _, _ in processes)
        preparation = result.get("preparation_cpu", {})
        failed_groups = [group for group in result["groups"] if group.get("failure_category")]
        pre_measurement_failure = "requested_window" not in result or (
            bool(failed_groups) and not any(group["measurement"].get("measurement_started") for group in failed_groups))
        verification = result.get("verification", {})
        server_side_authorization = verification.get("rejected") == 0 and (verification.get("handle_ms_max") or 0) < 1000
        tool_categories = ("CLIENT_RESOURCE", "CLIENT_STARTUP") if server_side_authorization else ("CLIENT_RESOURCE", "AUTHORIZATION", "CLIENT_STARTUP")
        if (result["status"] == "FAIL" and result.get("capacity_boundary") == "UNCONFIRMED" and pre_measurement_failure and
                (preparation.get("server_busy_max", 0) >= 0.95 * len(server_cpus) or preparation.get("server_max_cpu_busy", 0) >= 0.98) and
                preparation.get("client_max_cpu_busy", 1) < 0.95 and
                not any(group.get("failure_category") in tool_categories for group in result["groups"])):
            result["capacity_boundary"] = "SERVER_SATURATED"
        if result["cleanup"].get("errors") or result["cleanup"].get("remaining_owned_receivers"):
            result["status"] = "FAIL"
            result["capacity_boundary"] = "CLEANUP_FAILED"
        result["processes"] = [{"name": name, "pid": process.pid, "starttick": process_startticks.get(process.pid), "exit_code": process.returncode, "command": redacted_command(command)} for name, process, command, _ in processes]
        (args.output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({"status": result["status"], "result": str(args.output / "result.json"), "capacity_boundary": result.get("capacity_boundary")}))
    return 0 if result["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(run(parse_arguments()))
