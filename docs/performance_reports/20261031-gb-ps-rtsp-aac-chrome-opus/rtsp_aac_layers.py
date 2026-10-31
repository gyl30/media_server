#!/usr/bin/env python3
"""Follow AAC audio timestamps of one source through every layer: the fixture file, the RTP the FFmpeg publisher sends (UDP capture, RTSP UDP
input only), the server's internal frames as carried by GB PS PES timestamps (90 kHz), and the RTMP, HTTP-FLV and RTSP outputs as ffprobe sees
them. The GB elementary stream is also compared frame by frame with the fixture to tell timestamp changes from lost or repeated audio."""

import argparse
import collections
import json
import struct
import subprocess
import sys
import time
import uuid
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

PORTS = (11935, 18554, 18080)
CLOCK = 90000


def histogram(values, digits=1):
    return sorted(collections.Counter(round(value, digits) for value in values).items())


def steps(values):
    return [later - earlier for earlier, later in zip(values, values[1:])]


def probe_packets(ffprobe, url, seconds, env):
    command = [ffprobe, "-v", "error", "-select_streams", "a:0", "-show_entries", "packet=pts_time", "-of", "csv=p=0"]
    command += ["-rtsp_transport", "tcp"] if url.startswith("rtsp://") else []
    command += ["-read_intervals", f"%+{seconds}", url]
    process = subprocess.run(command, capture_output=True, text=True, env=env, timeout=seconds + 30)
    return [float(line.split(",")[0]) for line in process.stdout.splitlines() if line.strip() and line.split(",")[0] not in ("", "N/A")]


def summarize_ms(times_s):
    deltas = [value * 1000 for value in steps(times_s)]
    return {"frames": len(times_s), "step_ms_histogram": histogram(deltas)[:10],
            "steps_over_26ms": sum(value > 26 for value in deltas), "steps_under_16ms": sum(value < 16 for value in deltas)}


def rtp_audio(pcap):
    data = pcap.read_bytes()
    offset = 24
    link = struct.unpack_from("<I", data, 20)[0]
    packets = collections.defaultdict(list)
    while offset + 16 <= len(data):
        captured = struct.unpack_from("<I", data, offset + 8)[0]
        frame = data[offset + 16:offset + 16 + captured]
        offset += 16 + captured
        ip = frame[16:] if link == 113 else frame[14:] if link == 1 else frame[4:]
        if len(ip) < 28 or ip[0] >> 4 != 4 or ip[9] != 17:
            continue
        udp = ip[(ip[0] & 15) * 4:]
        rtp = udp[8:]
        if len(rtp) < 16 or rtp[0] >> 6 != 2 or 200 <= rtp[1] <= 204:
            continue
        packets[rtp[1] & 0x7F].append(rtp)
    for payload_type, items in packets.items():
        units = []
        for rtp in items:
            payload = rtp[12 + 4 * (rtp[0] & 15):]
            length = struct.unpack_from("!H", payload, 0)[0] if len(payload) >= 2 else 0
            if length == 0 or length % 16:
                break
            units.append(length // 16)
        else:
            stamps = [struct.unpack_from("!I", rtp, 4)[0] for rtp in items]
            deltas = steps(stamps)
            return {"payload_type": payload_type, "packets": len(items), "aus_per_packet_histogram": histogram(units, 0)[:10],
                    "timestamp_step_histogram": histogram(deltas, 0)[:10],
                    "step_minus_aus_x1024": histogram([delta - count * 1024 for delta, count in zip(deltas, units)], 0)[:10]}
    return {"error": "no mpeg4-generic RTP stream found"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--server-bin", default="build/media_server")
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--input", choices=("rtsp_udp", "rtsp_tcp", "rtmp"), default="rtsp_udp")
    parser.add_argument("--seconds", type=int, default=20)
    parser.add_argument("--host", default="198.19.0.10")
    parser.add_argument("--client-address", default="198.19.0.11")
    parser.add_argument("--server-cpus", default="0-3")
    parser.add_argument("--client-cpus", default="8-15")
    args = parser.parse_args()
    runtime = args.runtime.resolve()
    sys.path.insert(0, str(runtime / "bench"))
    from signaling_stub import SignalingStub
    from perf_run import request
    args.output.mkdir(parents=True)
    env = {"LD_LIBRARY_PATH": str(runtime / "lib")}
    ffmpeg, ffprobe = str(runtime / "bin/ffmpeg"), str(runtime / "bin/ffprobe")
    token = "rtsp-aac-layers"
    processes = []

    def launch(name, command, cpus):
        process = subprocess.Popen(["taskset", "-c", cpus, *map(str, command)], stdout=(args.output / f"{name}.log").open("w"),
                                   stderr=subprocess.STDOUT, env=env)
        processes.append(process)
        return process

    result = {"input": args.input}
    fixture_times = probe_packets(ffprobe, str(args.fixture), 60, env)
    result["fixture"] = summarize_ms(fixture_times)
    try:
        with SignalingStub() as stub:
            launch("server", [runtime / args.server_bin, "--threads", 2, "--bind-address", args.host, "--webrtc-address", args.host, "--rtmp-port",
                              PORTS[0], "--rtsp-port", PORTS[1], "--http-port", PORTS[2], "--control-token", token, "--signaling-url", stub.url],
                   args.server_cpus)
            time.sleep(1)
            capture = None
            if args.input == "rtsp_udp":
                capture = subprocess.Popen(["tcpdump", "-U", "-i", "lo", "-s", "256", "-w", str(args.output / "publish.pcap"),
                                            f"udp and dst host {args.host}"],
                                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                processes.append(capture)
                time.sleep(1)
            stream = stub.issue("publish")
            target = ["-f", "flv", f"rtmp://{args.host}:{PORTS[0]}/live/{stream}"] if args.input == "rtmp" else \
                ["-f", "rtsp", "-rtsp_transport", args.input[5:], f"rtsp://{args.host}:{PORTS[1]}/{stream}"]
            launch("publisher", [ffmpeg, "-nostdin", "-loglevel", "error", "-re", "-stream_loop", -1, "-i", args.fixture, "-c", "copy", *target],
                   args.client_cpus)
            base = f"http://{args.host}:{PORTS[2]}"
            for _ in range(100):
                if stream in {item["stream_id"] for item in request(base, "/receivers", token=token)["receivers"]}:
                    break
                time.sleep(0.2)
            (args.output / "control.token").write_text(token + "\n")
            (args.output / "gb.paths").write_text(f"{stream}/{uuid.uuid4()}\n")
            launch("gb", [runtime / "build/gb_fanout", "--play-paths-file", args.output / "gb.paths", "--media-host", args.host, "--media-port", PORTS[2],
                          "--control-token-file", args.output / "control.token", "--transport", "udp", "--bind-address", args.client_address,
                          "--viewers", 1, "--warmup", 1, "--duration", args.seconds + 5, "--ramp-per-second", 1, "--ready-timeout", 15,
                          "--dump-file", args.output / "gb.rtp"], args.client_cpus)
            urls = {"rtmp": f"rtmp://{args.host}:{PORTS[0]}/{stream}/{stub.issue('play', stream)}",
                    "flv": f"{base}/{stream}/{stub.issue('play', stream)}.flv",
                    "rtsp": f"rtsp://{args.host}:{PORTS[1]}/{stream}/{stub.issue('play', stream)}"}
            with ThreadPoolExecutor(3) as pool:
                futures = {name: pool.submit(probe_packets, ffprobe, url, args.seconds, env) for name, url in urls.items()}
                outputs = {name: future.result() for name, future in futures.items()}
            processes[-1].wait(timeout=args.seconds + 40)
    finally:
        for process in reversed(processes):
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=10)
    for name, times in outputs.items():
        result[name] = summarize_ms(times)
    if args.input == "rtsp_udp":
        result["publisher_rtp"] = rtp_audio(args.output / "publish.pcap")
    probe = subprocess.run([sys.executable, runtime / "bench/gb_av_probe.py", args.output / "gb.rtp", "--demux-bin", runtime / "build/gb_ps_demux",
                            "--ffmpeg-bin", ffmpeg, "--expect-audio", "--reference", args.fixture], capture_output=True, text=True, env=env)
    gb = json.loads(probe.stdout) if probe.stdout.strip() else {"error": probe.stderr[-300:]}
    rows = [line.split(",") for line in (args.output / "gb.csv").read_text().splitlines() if line.startswith("audio")]
    ticks = steps([int(row[1]) for row in rows])
    result["gb_internal"] = {"frames": len(rows), "step_ticks_histogram": histogram(ticks, 0)[:10],
                             "steps_over_26ms": sum(value > 26 * 90 for value in ticks), "steps_under_16ms": sum(value < 16 * 90 for value in ticks),
                             "first_long_step_frame": next((index + 1 for index, value in enumerate(ticks) if value > 26 * 90), None),
                             "audio_samples": gb.get("audio_samples"), "audio_expected_samples": gb.get("audio_expected_samples"),
                             "audio_pts_gaps": gb.get("audio_pts_gaps"), "audio_pts_slip_ms_max": gb.get("audio_pts_slip_ms_max"),
                             "audio_pts_slip_ms_min": gb.get("audio_pts_slip_ms_min"), "av_markers": gb.get("av_markers"),
                             "av_offset_ms_min": gb.get("av_offset_ms_min"), "av_offset_ms_max": gb.get("av_offset_ms_max"),
                             "reference": {key: (gb.get("reference") or {}).get(key) for key in ("matched", "unmatched", "jump_count", "ok")}}
    (args.output / "result.json").write_text(json.dumps(result, indent=1))
    print(json.dumps(result, indent=1))


if __name__ == "__main__":
    main()
