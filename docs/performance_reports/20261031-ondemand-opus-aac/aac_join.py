#!/usr/bin/env python3
"""Join AAC consumers one at a time to a WHIP or WHEP Pull source that otherwise has only a passthrough WHEP viewer.

Each join decodes one FLV/RTSP/RTMP output with av_probe while it is the only consumer of the source's AAC stream, with idle gaps between
joins, so every join is the first AAC consumer after none. Audio timestamp gaps are reported, not judged: the RTSP output shows one
at the client start in every build, so compare them with an always-on run. The passthrough viewer's RTP is replayed afterwards to check that its audio stayed
continuous, and the server debug log gives the number of Opus->AAC transcoder starts."""

import argparse
import json
import subprocess
import sys
import time
import uuid
from pathlib import Path

PORTS = (11935, 18554, 18080)
UPSTREAM_PORTS = (12035, 18654, 18180)
JOINS = ("flv", "rtsp", "rtmp")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--server-bin", default="build/media_server")
    parser.add_argument("--input", choices=("whip", "whep_pull"), default="whip")
    parser.add_argument("--host", default="198.19.0.10")
    parser.add_argument("--client-address", default="198.19.0.11")
    parser.add_argument("--server-cpus", default="0-3")
    parser.add_argument("--client-cpus", default="8-15")
    parser.add_argument("--lead", type=float, default=8, help="seconds with only the passthrough viewer before the first join")
    parser.add_argument("--probe", type=float, default=12, help="seconds each AAC consumer decodes")
    parser.add_argument("--gap", type=float, default=6, help="seconds without AAC consumers between joins")
    parser.add_argument("--expect-restarts", type=int, choices=(0, 1), required=True,
                        help="Opus->AAC transcoder starts expected at each join: 1 for on-demand transcoding, 0 for always-on")
    args = parser.parse_args()
    runtime = args.runtime.resolve()
    sys.path.insert(0, str(runtime / "bench"))
    from signaling_stub import SignalingStub
    from perf_run import request
    args.output.mkdir(parents=True)
    env = {"LD_LIBRARY_PATH": str(runtime / "lib")}
    log_path = args.output / "server.log"
    processes = []
    token = "aac-join"

    def launch(name, command, cpus, **extra):
        process = subprocess.Popen(["taskset", "-c", cpus, *map(str, command)], stdout=(args.output / f"{name}.log").open("w"),
                                   stderr=subprocess.STDOUT, env={**env, **extra.pop("env", {})}, **extra)
        processes.append(process)
        return process

    def server(name, host, ports, cpus, stub):
        launch(name, [runtime / args.server_bin, "--threads", 2, "--bind-address", host, "--webrtc-address", host, "--rtmp-port", ports[0],
                      "--rtsp-port", ports[1], "--http-port", ports[2], "--control-token", token, "--signaling-url", stub.url], cpus,
               env={"MEDIA_SERVER_LOG_LEVEL": "debug"})
        time.sleep(1)

    def wait_receiver(base, stream):
        for _ in range(100):
            if stream in {item["stream_id"] for item in request(base, "/receivers", token=token)["receivers"]}:
                return
            time.sleep(0.2)
        sys.exit(f"source {stream} not ready")

    def play_url(protocol, stream, stub, host=args.host, ports=PORTS):
        path = f"{stream}/{stub.issue('play', stream)}"
        return {"flv": f"http://{host}:{ports[2]}/{path}.flv", "rtsp": f"rtsp://{host}:{ports[1]}/{path}", "rtmp": f"rtmp://{host}:{ports[0]}/{path}",
                "whep": f"http://{host}:{ports[2]}/play/whep/{path}"}[protocol]

    result = {"input": args.input, "server_bin": args.server_bin, "joins": []}
    with SignalingStub() as stub:
        server("server", args.host, PORTS, args.server_cpus, stub)
        control = f"http://{args.host}:{PORTS[2]}"
        if args.input == "whip":
            stream = stub.issue("publish")
            (args.output / "whip.urls").write_text(f"http://{args.host}:{PORTS[2]}/publish/whip/{stream}\n")
            launch("whip_publisher", [runtime / "build/whip_publisher", "--whip-urls-file", args.output / "whip.urls", "--fixture",
                                      runtime / "fixtures/avsync_whip.mkv", "--sources", 1, "--bind-address", args.client_address, "--duration", 86400],
                   args.client_cpus)
        else:
            server("upstream", args.client_address, UPSTREAM_PORTS, args.client_cpus, stub)
            upstream = stub.issue("publish")
            launch("ffmpeg_publisher", [runtime / "bin/ffmpeg", "-nostdin", "-loglevel", "error", "-re", "-stream_loop", -1, "-i",
                                        runtime / "fixtures/avsync_720p30.flv", "-c", "copy", "-f", "flv",
                                        f"rtmp://{args.client_address}:{UPSTREAM_PORTS[0]}/live/{upstream}"], args.client_cpus)
            wait_receiver(f"http://{args.client_address}:{UPSTREAM_PORTS[2]}", upstream)
            stream = str(uuid.uuid4())
            request(control, "/pull/create", {"stream_id": stream,
                                              "url": play_url("whep", upstream, stub, args.client_address, UPSTREAM_PORTS)}, token)
        wait_receiver(control, stream)
        time.sleep(2)
        (args.output / "whep.urls").write_text(play_url("whep", stream, stub) + "\n")
        total = args.lead + len(JOINS) * (args.probe + args.gap) + 10
        viewer = launch("whep_fanout", [runtime / "build/whep_fanout", "--whep-urls-file", args.output / "whep.urls", "--viewers", 1,
                                        "--ramp-per-second", 1, "--io-threads", 1, "--warmup", 1, "--duration", int(total), "--ready-timeout", 15,
                                        "--bind-address", args.client_address, "--dump-file", args.output / "whep.dump"], args.client_cpus)
        time.sleep(args.lead)
        for protocol in JOINS:
            before = log_path.read_text(errors="replace").count("audio transcoder started input opus")
            probe = subprocess.run([sys.executable, runtime / "bench/av_probe.py", "--seconds", str(args.probe), "--ffmpeg-bin", runtime / "bin/ffmpeg",
                                    play_url(protocol, stream, stub)], capture_output=True, text=True, env=env)
            item = {"protocol": protocol, "probe_exit": probe.returncode}
            item.update(json.loads(probe.stdout.strip().splitlines()[-1]) if probe.stdout.strip() else {"error": probe.stderr[-300:]})
            item["transcoder_starts_during_join"] = log_path.read_text(errors="replace").count("audio transcoder started input opus") - before
            result["joins"].append(item)
            print(json.dumps({key: item.get(key) for key in ("protocol", "first_video_s", "first_audio_s", "audio_start_after_video_ms", "audio_frames",
                                                             "audio_pts_gaps", "av_markers", "av_offset_ms_p50", "decode_warnings",
                                                             "transcoder_starts_during_join")}), flush=True)
            time.sleep(args.gap)
        viewer.wait(timeout=total + 30)
        result["whep_viewer_exit"] = viewer.returncode
        replay = subprocess.run([sys.executable, runtime / "bench/whep_dump_av.py", args.output / "whep.dump", "--output", args.output / "whep.mkv",
                                 "--ffmpeg-bin", runtime / "bin/ffmpeg"], capture_output=True, text=True, env=env)
        result["whep_viewer"] = json.loads(replay.stdout.strip().splitlines()[-1]) if replay.stdout.strip() else {"error": replay.stderr[-300:]}
        for process in reversed(processes):
            process.kill()
            process.wait(timeout=10)
    text = log_path.read_text(errors="replace")
    result["transcoder_starts_total"] = text.count("audio transcoder started input opus")
    result["egress_starts_total"] = text.count("audio transcoder started input aac")
    whep = result["whep_viewer"]
    result["checks"] = {
        "joins_have_audio": all(item.get("audio_frames", 0) > 0 and item.get("av_markers", 0) > 0 for item in result["joins"]),
        "joins_restart_transcoder": all(item["transcoder_starts_during_join"] == args.expect_restarts for item in result["joins"]),
        "joins_decode_clean": all(item.get("decode_warnings") == 0 and item.get("video_pts_backward") == 0 for item in result["joins"]),
        "whep_viewer_continuous": whep.get("audio_pts_gaps") == 0 and whep.get("video_pts_backward") == 0 and whep.get("av_markers", 0) > 0,
    }
    (args.output / "result.json").write_text(json.dumps(result, indent=1))
    print(json.dumps({key: result[key] for key in ("transcoder_starts_total", "egress_starts_total", "checks")}))
    print("AAC_JOIN", "PASS" if all(result["checks"].values()) else "FAIL")


if __name__ == "__main__":
    main()
