#!/usr/bin/env python3

import argparse
import json
import os
import platform
import statistics
import subprocess
import threading
import time
from pathlib import Path

from fanout_support import benchmark_head, proc_snapshot, stop_process, thread_rates, wait_for_listener, wait_for_stream


class flv_counter:
    def __init__(self, pipe):
        self.pipe = pipe
        self.video_bytes = 0
        self.audio_bytes = 0
        self.video_tags = 0
        self.audio_tags = 0
        self.thread = threading.Thread(target=self.read, daemon=True)
        self.thread.start()

    def read(self):
        buffer = bytearray()
        header_read = False
        while chunk := os.read(self.pipe.fileno(), 64 * 1024):
            buffer.extend(chunk)
            if not header_read:
                if len(buffer) < 13:
                    continue
                header_size = int.from_bytes(buffer[5:9], "big")
                if len(buffer) < header_size + 4:
                    continue
                del buffer[:header_size + 4]
                header_read = True
            while len(buffer) >= 15:
                payload_size = int.from_bytes(buffer[1:4], "big")
                tag_size = 11 + payload_size + 4
                if len(buffer) < tag_size:
                    break
                if buffer[0] == 9:
                    self.video_bytes += payload_size
                    self.video_tags += 1
                elif buffer[0] == 8:
                    self.audio_bytes += payload_size
                    self.audio_tags += 1
                del buffer[:tag_size]

    def snapshot(self):
        return self.video_bytes, self.audio_bytes, self.video_tags, self.audio_tags


def quantiles(values):
    ordered = sorted(values)
    return {point: ordered[int(fraction * (len(ordered) - 1))]
            for point, fraction in (("min", 0), ("p10", 0.1), ("p50", 0.5), ("p90", 0.9), ("max", 1))}


def loopback_packets():
    for line in Path("/proc/net/dev").read_text().splitlines():
        if line.strip().startswith("lo:"):
            return int(line.split(":", 1)[1].split()[9])
    raise RuntimeError("loopback interface missing")


def main():
    parser = argparse.ArgumentParser(description="RTMP AV1 fanout with FourCC-capable FFmpeg clients")
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--server-bin", type=Path, default=Path("build/media_server"))
    parser.add_argument("--ffmpeg-bin", type=Path, default=Path("bin/ffmpeg"))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--viewers", type=int, required=True)
    parser.add_argument("--workers", type=int, default=6)
    parser.add_argument("--ramp-per-second", type=int, default=10)
    parser.add_argument("--warmup", type=int, default=5)
    parser.add_argument("--duration", type=int, default=20)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--rtmp-port", type=int, default=11935)
    parser.add_argument("--rtsp-port", type=int, default=18554)
    parser.add_argument("--http-port", type=int, default=18080)
    args = parser.parse_args()
    if min(args.viewers, args.workers, args.ramp_per_second, args.duration) < 1 or args.warmup < 0:
        parser.error("viewers, workers, ramp-per-second and duration must be positive")

    args.output.mkdir(parents=True, exist_ok=True)
    server = None
    publisher = None
    clients = []
    logs = []
    try:
        server_log = (args.output / "server.log").open("w")
        logs.append(server_log)
        server = subprocess.Popen(
            [str(args.server_bin), "--threads", str(args.workers), "--rtmp-port", str(args.rtmp_port),
             "--rtsp-port", str(args.rtsp_port), "--http-port", str(args.http_port), "--bind-address", args.host,
             "--webrtc-address", args.host, "--rtmp-video-codec", "av1"], stdout=server_log, stderr=subprocess.STDOUT,
        )
        wait_for_listener(args.host, args.rtmp_port)
        publisher_log = (args.output / "publisher.log").open("w")
        logs.append(publisher_log)
        publisher = subprocess.Popen(
            [str(args.ffmpeg_bin), "-hide_banner", "-loglevel", "error", "-stream_loop", "-1", "-re", "-i", str(args.fixture),
             "-c", "copy", "-f", "flv", f"rtmp://{args.host}:{args.rtmp_port}/live/perf0"],
            stdout=publisher_log, stderr=subprocess.STDOUT,
        )
        wait_for_stream(args.host, args.http_port)
        for index in range(args.viewers):
            client_log = (args.output / f"client-{index}.log").open("w")
            logs.append(client_log)
            client = subprocess.Popen(
                [str(args.ffmpeg_bin), "-hide_banner", "-loglevel", "error", "-rtmp_enhanced_codecs", "av01",
                 "-i", f"rtmp://{args.host}:{args.rtmp_port}/live/perf0", "-c", "copy", "-f", "flv", "pipe:1"],
                stdout=subprocess.PIPE, stderr=client_log,
            )
            clients.append((client, flv_counter(client.stdout)))
            if index + 1 < args.viewers:
                time.sleep(1 / args.ramp_per_second)

        ready_deadline = time.monotonic() + 45
        while time.monotonic() < ready_deadline:
            if any(client.poll() is not None for client, _ in clients):
                raise RuntimeError("RTMP AV1 client exited before media was ready")
            if all(counter.video_tags > 0 and counter.audio_tags > 0 for _, counter in clients):
                break
            time.sleep(0.1)
        else:
            raise RuntimeError("RTMP AV1 viewers did not receive both media tracks")

        time.sleep(args.warmup)
        before_server = proc_snapshot(server.pid)
        before_packets = loopback_packets()
        before_clients = [proc_snapshot(client.pid) for client, _ in clients]
        before_media = [counter.snapshot() for _, counter in clients]
        server_samples = []
        started = time.monotonic()
        while time.monotonic() - started < args.duration:
            time.sleep(min(1, max(0, args.duration - (time.monotonic() - started))))
            server_samples.append(proc_snapshot(server.pid))
        elapsed = time.monotonic() - started
        after_server = proc_snapshot(server.pid)
        after_packets = loopback_packets()
        after_clients = [proc_snapshot(client.pid) for client, _ in clients]
        after_media = [counter.snapshot() for _, counter in clients]
        deltas = [tuple(after - before for before, after in zip(initial, final))
                  for initial, final in zip(before_media, after_media)]
        progressing = sum(video > 0 and audio > 0 and video_tags > 0 and audio_tags > 0
                          for video, audio, video_tags, audio_tags in deltas)
        result = {
            "config": {"head": benchmark_head(), "fixture": str(args.fixture), "viewers": args.viewers,
                       "workers": args.workers, "warmup_seconds": args.warmup, "duration_seconds": args.duration,
                       "ramp_per_second": args.ramp_per_second, "kernel": platform.release()},
            "ready": len(clients), "progressing": progressing,
            "aggregate_gbit_per_second": 8 * sum(video + audio for video, audio, _, _ in deltas) / (elapsed * 1e9),
            "video_tags_per_second": sum(value[2] for value in deltas) / elapsed,
            "audio_tags_per_second": sum(value[3] for value in deltas) / elapsed,
            "viewer_video_bytes_per_second": quantiles([value[0] / elapsed for value in deltas]),
            "viewer_audio_bytes_per_second": quantiles([value[1] / elapsed for value in deltas]),
            "viewer_video_tags": quantiles([value[2] for value in deltas]),
            "viewer_audio_tags": quantiles([value[3] for value in deltas]),
            "server_cpu_cores": (after_server["cpu"] - before_server["cpu"]) / elapsed,
            "server_pss_kib_median": statistics.median(sample["pss_kib"] for sample in server_samples),
            "server_fd_median": statistics.median(sample["fd"] for sample in server_samples),
            "server_thread_cpu_cores": thread_rates(before_server, after_server, "cpu", elapsed),
            "loopback_tx_packets_per_second": (after_packets - before_packets) / elapsed,
            "client_cpu_cores": sum(after["cpu"] - before["cpu"] for before, after in zip(before_clients, after_clients)) / elapsed,
        }
        (args.output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
        print(json.dumps(result, indent=2))
        if progressing != args.viewers:
            raise SystemExit(1)
    finally:
        for client, counter in clients:
            stop_process(client)
            counter.thread.join(timeout=2)
        stop_process(publisher)
        stop_process(server)
        for log in logs:
            log.close()


if __name__ == "__main__":
    main()
