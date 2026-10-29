#!/usr/bin/env python3
"""Stage 1 smoke driver: one source and one viewer per protocol over authenticated stream tokens.

The media server verifies every publish and play against the signaling service and consumes the
token, so each session needs its own token. This driver starts the signaling stub, the server,
one publisher and the requested viewer, then judges health by continuous media growth.
"""

import argparse
import json
import socket
import statistics
import subprocess
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from fanout_support import proc_snapshot, stop_process, wait_for_listener  # noqa: E402
from signaling_stub import SignalingStub  # noqa: E402

PUBLISH_WAIT_SECONDS = 20
SAMPLE_INTERVAL_SECONDS = 1.0


def probe_status(port, host, path, timeout=10):
    """Read only the response head; a live media response keeps streaming forever."""
    connection = socket.create_connection((host, port), timeout=timeout)
    try:
        connection.sendall(f"GET {path} HTTP/1.1\r\nHost: {host}\r\nConnection: close\r\n\r\n".encode())
        head = b""
        while b"\r\n\r\n" not in head:
            chunk = connection.recv(4096)
            if not chunk:
                break
            head += chunk
        if not head:
            return None
        return int(head.split(b"\r\n", 1)[0].decode("latin-1").split(" ")[1])
    finally:
        connection.close()


def follow_redirect(port, host, path):
    """The HLS entry point answers 307 with the session playlist location."""
    seen = path
    for _ in range(4):
        connection = socket.create_connection((host, port), timeout=10)
        try:
            connection.sendall(f"GET {seen} HTTP/1.1\r\nHost: {host}\r\nConnection: close\r\n\r\n".encode())
            head = b""
            while b"\r\n\r\n" not in head:
                chunk = connection.recv(4096)
                if not chunk:
                    break
                head += chunk
            lines = head.split(b"\r\n")
            status = int(lines[0].decode("latin-1").split(" ")[1])
            if status not in (301, 302, 307):
                return seen, status
            for line in lines[1:]:
                name, _, value = line.decode("latin-1").partition(":")
                if name.strip().lower() == "location":
                    seen = value.strip()
                    break
            else:
                raise RuntimeError(f"redirect without location for {seen}")
        finally:
            connection.close()
    raise RuntimeError(f"too many redirects for {path}")


def http_request(port, host, path, timeout=10):
    connection = socket.create_connection((host, port), timeout=timeout)
    try:
        connection.sendall(f"GET {path} HTTP/1.1\r\nHost: {host}\r\nConnection: close\r\n\r\n".encode())
        head = b""
        while b"\r\n\r\n" not in head:
            chunk = connection.recv(65536)
            if not chunk:
                return None, b""
            head += chunk
        header, _, body = head.partition(b"\r\n\r\n")
        status = int(header.decode("latin-1").split("\r\n")[0].split(" ")[1])
        while True:
            chunk = connection.recv(65536)
            if not chunk:
                break
            body += chunk
        return status, body
    finally:
        connection.close()


def pull_progressive(port, host, path, duration, log):
    """Download one continuous media resource and record byte growth per second."""
    connection = socket.create_connection((host, port), timeout=10)
    connection.sendall(f"GET {path} HTTP/1.1\r\nHost: {host}\r\nConnection: close\r\n\r\n".encode())
    first = connection.recv(16)
    if not first.startswith(b"HTTP/1.1 200"):
        raise RuntimeError(f"unexpected response prefix {first!r}")
    deadline = time.monotonic() + duration
    total = len(first)
    marks = []
    next_mark = time.monotonic()
    while time.monotonic() < deadline:
        chunk = connection.recv(262144)
        if not chunk:
            break
        total += len(chunk)
        if time.monotonic() >= next_mark:
            marks.append(total)
            log.write(f"phase=measure bytes={total}\n")
            log.flush()
            next_mark += 1.0
    connection.close()
    return total, marks


def pull_hls(port, host, playlist_path, duration, log):
    """Follow the HLS playlist and download segments for the whole window."""
    total = 0
    deadline = time.monotonic() + duration
    marks = []
    while time.monotonic() < deadline:
        playlist_path, status = follow_redirect(port, host, playlist_path)
        if status != 200:
            raise RuntimeError(f"hls playlist status {status}")
        status, body = http_request(port, host, playlist_path)
        if status != 200:
            raise RuntimeError(f"hls playlist reload status {status}")
        total += len(body)
        for line in body.decode("latin-1").splitlines():
            if not line or line.startswith("#"):
                continue
            segment = line if line.startswith("/") else playlist_path.rsplit("/", 1)[0] + "/" + line
            status, body = http_request(port, host, segment)
            if status != 200:
                raise RuntimeError(f"hls segment status {status} path {segment}")
            total += len(body)
        marks.append(total)
        log.write(f"phase=measure bytes={total}\n")
        log.flush()
        time.sleep(1)
    return total, marks


def main():
    parser = argparse.ArgumentParser(description="Authenticated single-viewer protocol smoke")
    parser.add_argument("--case", required=True,
                        choices=("rtmp_publish", "rtsp_publish_tcp", "rtsp_publish_udp",
                                 "rtmp_play", "rtsp_play", "flv_play", "hls_play", "whep_play"))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--server-bin", type=Path, required=True)
    parser.add_argument("--ffmpeg-bin", type=Path, required=True)
    parser.add_argument("--client-bin", type=Path, default=Path("."))
    parser.add_argument("--host", default="198.19.0.10")
    parser.add_argument("--client-bind-address", default="198.19.0.11")
    parser.add_argument("--rtmp-port", type=int, default=11935)
    parser.add_argument("--rtsp-port", type=int, default=18554)
    parser.add_argument("--http-port", type=int, default=18080)
    parser.add_argument("--workers", type=int, default=8)
    parser.add_argument("--warmup", type=float, default=5)
    parser.add_argument("--duration", type=float, default=15)
    args = parser.parse_args()

    args.output.mkdir(parents=True, exist_ok=True)
    server_log = (args.output / "server.log").open("w")
    publisher_log = (args.output / "publisher.log").open("w")
    client_log = (args.output / "client.log").open("w")
    measure_log = (args.output / "measure.log").open("w")

    server = None
    publisher = None
    client = None
    samples = []
    stop_sampling = threading.Event()
    result = {"case": args.case, "status": "FAIL",
              "head": __import__("os").environ.get("MEDIA_SERVER_BENCH_HEAD", "unknown")}

    def sample_loop(pid):
        while not stop_sampling.wait(SAMPLE_INTERVAL_SECONDS):
            try:
                samples.append(proc_snapshot(pid))
            except (FileNotFoundError, ProcessLookupError):
                stop_sampling.set()

    try:
        with SignalingStub() as signaling:
            server = subprocess.Popen(
                [str(args.server_bin), "--threads", str(args.workers),
                 "--rtmp-port", str(args.rtmp_port), "--rtsp-port", str(args.rtsp_port),
                 "--http-port", str(args.http_port), "--bind-address", args.host,
                 "--webrtc-address", args.host, "--signaling-url", signaling.url],
                stdout=server_log, stderr=subprocess.STDOUT)
            for port in (args.http_port, args.rtmp_port, args.rtsp_port):
                wait_for_listener(args.host, port)

            stream_id = signaling.issue("publish")
            result["stream_id"] = stream_id
            if args.case.startswith("rtsp_publish"):
                transport = "udp" if args.case.endswith("udp") else "tcp"
                publisher = subprocess.Popen(
                    [str(args.ffmpeg_bin), "-hide_banner", "-loglevel", "warning", "-re", "-stream_loop", "-1",
                     "-i", str(args.fixture), "-c", "copy", "-rtsp_transport", transport, "-f", "rtsp",
                     f"rtsp://{args.host}:{args.rtsp_port}/{stream_id}"],
                    stdout=publisher_log, stderr=subprocess.STDOUT)
            else:
                publisher = subprocess.Popen(
                    [str(args.ffmpeg_bin), "-hide_banner", "-loglevel", "warning", "-re", "-stream_loop", "-1",
                     "-i", str(args.fixture), "-c", "copy", "-f", "flv",
                     f"rtmp://{args.host}:{args.rtmp_port}/live/{stream_id}"],
                    stdout=publisher_log, stderr=subprocess.STDOUT)

            def source_readable():
                status = probe_status(args.http_port, args.host,
                                      f"/{stream_id}/{signaling.issue('play', stream_id)}.flv")
                result["last_probe_status"] = status
                return status == 200

            deadline = time.monotonic() + PUBLISH_WAIT_SECONDS
            while not source_readable():
                if time.monotonic() > deadline:
                    raise RuntimeError("source did not become readable after publish")
                time.sleep(0.5)

            time.sleep(args.warmup)
            sampler = threading.Thread(target=sample_loop, args=(server.pid,), daemon=True)
            window_start = time.monotonic()
            sampler.start()

            media_bytes = None
            growth_marks = []
            client_returncode = 0
            if args.case in ("rtmp_publish", "rtsp_publish_tcp", "rtsp_publish_udp"):
                time.sleep(args.duration)
            elif args.case == "flv_play":
                media_bytes, growth_marks = pull_progressive(
                    args.http_port, args.host,
                    f"/{stream_id}/{signaling.issue('play', stream_id)}.flv", args.duration, measure_log)
            elif args.case == "hls_play":
                media_bytes, growth_marks = pull_hls(
                    args.http_port, args.host,
                    f"/play/hls/{stream_id}/{signaling.issue('play', stream_id)}/index.m3u8",
                    args.duration, measure_log)
            elif args.case == "whep_play":
                urls = args.output / "whep-urls.txt"
                urls.write_text(f"http://{args.host}:{args.http_port}/play/whep/{stream_id}/"
                                f"{signaling.issue('play', stream_id)}\n")
                client = subprocess.Popen(
                    [str(args.client_bin / "whep_fanout"), "--whep-urls-file", str(urls),
                     "--bind-address", args.client_bind_address, "--viewers", "1", "--sources", "1",
                     "--warmup", "0", "--duration", str(int(args.duration))],
                    stdout=client_log, stderr=subprocess.STDOUT)
                client_returncode = client.wait()
            else:
                play_port = args.rtmp_port if args.case == "rtmp_play" else args.rtsp_port
                client_binary = "rtmp_fanout" if args.case == "rtmp_play" else "rtsp_fanout"
                client = subprocess.Popen(
                    [str(args.client_bin / client_binary), "--stream-name",
                     f"{stream_id}/{signaling.issue('play', stream_id)}",
                     "--media-host", args.host, "--media-port", str(play_port), "--viewers", "1",
                     "--warmup", "0", "--duration", str(int(args.duration))],
                    stdout=client_log, stderr=subprocess.STDOUT)
                client_returncode = client.wait()

            elapsed = max(1e-6, time.monotonic() - window_start)
            stop_sampling.set()
            sampler.join(timeout=5)
            after_server = proc_snapshot(server.pid)
            before_server = samples[0] if samples else after_server

            still_readable = source_readable()
            stop_process(publisher)
            publisher = None

            result.update({
                "window_seconds": round(elapsed, 3),
                "server_cpu_cores": round((after_server["cpu"] - before_server["cpu"]) / elapsed, 4),
                "server_pss_kib_median": statistics.median([s["pss_kib"] for s in samples]) if samples else 0,
                "server_pss_kib_peak": max([s["pss_kib"] for s in samples], default=0),
                "server_rss_kib_median": statistics.median([s["rss_kib"] for s in samples]) if samples else 0,
                "server_fd_median": statistics.median([s["fd"] for s in samples]) if samples else 0,
                "server_udp_drops": after_server["udp_drops"] - before_server["udp_drops"],
                "media_bytes": media_bytes,
                "media_growth_marks": growth_marks,
                "client_returncode": client_returncode,
                "source_readable_after_window": still_readable,
            })
            growing = media_bytes is None or len(growth_marks) >= 2
            result["status"] = "PASS" if (client_returncode == 0 and still_readable and growing) else "FAIL"
            stop_process(server)
            server = None
    except Exception as error:  # noqa: BLE001 - keep the raw failure as evidence
        import traceback
        result["error"] = f"{type(error).__name__}: {error}\n{traceback.format_exc()}"
    finally:
        stop_sampling.set()
        for process in (client, publisher, server):
            if process is not None:
                stop_process(process)
        for handle in (server_log, publisher_log, client_log, measure_log):
            handle.close()
        (args.output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
        print(json.dumps(result, indent=2))
        raise SystemExit(0 if result["status"] == "PASS" else 1)


if __name__ == "__main__":
    main()
