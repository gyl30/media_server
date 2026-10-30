#!/usr/bin/env python3
"""Offer one WHEP viewer per Opus fmtp variant against a whip_publisher stereo source and record whether the server forwarded
the raw Opus or started an AAC->Opus egress (debug log line), plus whether the viewer reached media. Variants may append SDP
lines after the fmtp (e.g. a=maxptime) because whep_fanout writes --opus-fmtp verbatim."""

import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

VARIANTS = [
    ("whep_fanout default", "minptime=10;useinbandfec=1;stereo=1", "passthrough"),
    ("Chrome 140/156 default", "minptime=10;useinbandfec=1", "egress 1ch"),
    ("Firefox 141 default", "maxplaybackrate=48000;stereo=1;useinbandfec=1", "passthrough"),
    ("stereo=0", "minptime=10;useinbandfec=1;stereo=0", "egress 1ch"),
    ("maxplaybackrate=24000", "stereo=1;maxplaybackrate=24000", "egress 2ch"),
    ("maxplaybackrate=96000 (out of range)", "stereo=1;maxplaybackrate=96000", "egress 2ch"),
    ("maxaveragebitrate=510000", "stereo=1;maxaveragebitrate=510000", "passthrough"),
    ("maxaveragebitrate=128000", "stereo=1;maxaveragebitrate=128000", "egress 2ch"),
    ("maxaveragebitrate=abc (malformed)", "stereo=1;maxaveragebitrate=abc", "egress 2ch"),
    ("cbr=1 (receiver preference)", "stereo=1;cbr=1", "passthrough"),
    ("a=maxptime:120", "stereo=1\r\na=maxptime:120", "passthrough"),
    ("a=maxptime:60", "stereo=1\r\na=maxptime:60", "egress 2ch"),
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--host", default="198.19.0.10")
    parser.add_argument("--client-address", default="198.19.0.11")
    parser.add_argument("--server-cpus", default="0-1")
    parser.add_argument("--client-cpus", default="8-15")
    args = parser.parse_args()
    sys.path.insert(0, str(args.runtime / "bench"))
    from signaling_stub import SignalingStub
    args.output.mkdir(parents=True)
    env = {"LD_LIBRARY_PATH": str(args.runtime / "lib")}
    log_path = args.output / "server.log"
    results = []
    processes = []
    with SignalingStub() as stub:
        processes.append(subprocess.Popen(["taskset", "-c", args.server_cpus, str(args.runtime / "build/media_server"), "--threads", "2", "--bind-address", args.host,
                                           "--webrtc-address", args.host, "--rtmp-port", "11935", "--rtsp-port", "18554", "--http-port", "18080",
                                           "--control-token", "sdp-boundary", "--signaling-url", stub.url], stdout=log_path.open("w"), stderr=subprocess.STDOUT,
                                          env={**env, "MEDIA_SERVER_LOG_LEVEL": "debug"}))
        time.sleep(1)
        whip = stub.issue("publish")
        (args.output / "whip.urls").write_text(f"http://{args.host}:18080/publish/whip/{whip}\n")
        processes.append(subprocess.Popen(["taskset", "-c", args.client_cpus, str(args.runtime / "build/whip_publisher"), "--whip-urls-file", str(args.output / "whip.urls"),
                                           "--fixture", str(args.runtime / ".cache/perf_fixture_whip.mkv"), "--sources", "1", "--bind-address", args.client_address,
                                           "--duration", "86400"], stdout=(args.output / "whip_publisher.log").open("w"), stderr=subprocess.STDOUT, env=env))
        time.sleep(4)
        for index, (name, fmtp, expected) in enumerate(VARIANTS):
            urls = args.output / f"viewer-{index}.urls"
            urls.write_text(f"http://{args.host}:18080/play/whep/{whip}/{stub.issue('play', whip)}\n")
            offset = log_path.stat().st_size
            viewer = subprocess.run(["taskset", "-c", args.client_cpus, str(args.runtime / "build/whep_fanout"), "--whep-urls-file", str(urls), "--viewers", "1",
                                     "--ramp-per-second", "1", "--io-threads", "1", "--warmup", "1", "--duration", "4", "--ready-timeout", "15",
                                     "--bind-address", args.client_address, "--opus-fmtp", fmtp], capture_output=True, text=True, env=env)
            time.sleep(1)
            with log_path.open() as log:
                log.seek(offset)
                egress = [line.strip() for line in log if "audio transcoder started input aac" in line]
            observed = "passthrough" if not egress else f"egress {'2ch' if 'output opus 48000hz 2ch' in egress[0] else '1ch'}"
            item = {"variant": name, "fmtp": fmtp, "expected": expected, "observed": observed, "viewer_exit": viewer.returncode,
                    "media_ready": "phase=media_ready" in viewer.stdout + viewer.stderr, "egress_log": egress}
            results.append(item)
            print(json.dumps({k: item[k] for k in ("variant", "expected", "observed", "viewer_exit", "media_ready")}), flush=True)
        for process in reversed(processes):
            process.kill()
            process.wait(timeout=10)
    (args.output / "result.json").write_text(json.dumps(results, indent=1))
    print("SDP_BOUNDARY", "PASS" if all(r["expected"] == r["observed"] and r["media_ready"] for r in results) else "FAIL")


if __name__ == "__main__":
    main()
