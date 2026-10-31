#!/usr/bin/env python3
"""Publish mono, stereo and mono->stereo->mono Opus over WHIP and play each source at once with whep_fanout viewers offering stereo=0,
Chrome's default fmtp and Firefox's default fmtp, a real Chrome over WHEP and an HTTP-FLV decoder.

For every whep_fanout viewer it reports the Opus TOC stereo flags received per time bucket, whether an AAC->Opus egress served it, and the
replayed decode; for Chrome it samples inbound audio stats; for FLV it reports av_probe. Run it against the production build and against an
experiment build to compare passthrough policies."""

import argparse
import asyncio
import json
import struct
import subprocess
import sys
import threading
import time
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

PORTS = (11935, 18554, 18080)
FIXTURES = ("c-mono.mkv", "c-stereo.mkv", "c-switch.mkv")
OFFERS = {"stereo0": "minptime=10;useinbandfec=1;stereo=0", "chrome": "minptime=10;useinbandfec=1",
          "firefox": "maxplaybackrate=48000;stereo=1;useinbandfec=1"}
BUCKET_SECONDS = 10
PAGE_JS = """async () => {
  window.pc = new RTCPeerConnection();
  pc.addTransceiver('video', {direction: 'recvonly'}); pc.addTransceiver('audio', {direction: 'recvonly'});
  pc.ontrack = e => { const el = document.createElement(e.track.kind); el.srcObject = new MediaStream([e.track]); el.autoplay = true;
                      document.body.appendChild(el); el.play().catch(() => {}); };
  await pc.setLocalDescription(await pc.createOffer());
  await new Promise(r => { if (pc.iceGatheringState === 'complete') r(); pc.onicegatheringstatechange = () => pc.iceGatheringState === 'complete' && r(); setTimeout(r, 2000); });
  return pc.localDescription.sdp; }"""
STATS_JS = """async () => {
  const out = {connection: pc.connectionState};
  (await pc.getStats()).forEach(s => {
    if (s.type === 'inbound-rtp' && s.kind === 'audio') Object.assign(out, {codecId: s.codecId, packetsReceived: s.packetsReceived,
      packetsLost: s.packetsLost, totalSamplesReceived: s.totalSamplesReceived, concealedSamples: s.concealedSamples,
      concealmentEvents: s.concealmentEvents, audioLevel: s.audioLevel, totalAudioEnergy: s.totalAudioEnergy});
  });
  (await pc.getStats()).forEach(s => { if (s.type === 'codec' && s.id === out.codecId) Object.assign(out, {channels: s.channels, fmtp: s.sdpFmtpLine}); });
  return out; }"""


def toc_buckets(path):
    data = path.read_bytes() if path.exists() else b""
    buckets = {}
    first = None
    offset = 0
    while offset + 11 <= len(data):
        nanoseconds, rtcp, size = struct.unpack_from("<qBH", data, offset)
        packet = data[offset + 11:offset + 11 + size]
        offset += 11 + size
        if rtcp or len(packet) < 13 or packet[1] & 0x7F != 111:
            continue
        header = 12 + 4 * (packet[0] & 0x0F)
        if packet[0] & 0x10:
            header += 4 + 4 * struct.unpack_from("!H", packet, header + 2)[0]
        if header >= len(packet):
            continue
        first = nanoseconds if first is None else first
        bucket = buckets.setdefault(str(int((nanoseconds - first) / 1e9 // BUCKET_SECONDS * BUCKET_SECONDS)), {"mono": 0, "stereo": 0})
        bucket["stereo" if packet[header] & 0x04 else "mono"] += 1
    return buckets


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--server-bin", required=True)
    parser.add_argument("--fixtures", type=Path, required=True, help="directory with c-mono.mkv, c-stereo.mkv, c-switch.mkv")
    parser.add_argument("--chrome-bin", type=Path, required=True)
    parser.add_argument("--seconds", type=int, default=100)
    parser.add_argument("--host", default="198.19.0.10")
    parser.add_argument("--client-address", default="198.19.0.11")
    parser.add_argument("--server-cpus", default="0-3")
    parser.add_argument("--client-cpus", default="8-15")
    parser.add_argument("--browser-cpus", default="24-31")
    args = parser.parse_args()
    processes = []
    try:
        asyncio.run(run(args, processes))
    finally:
        for process in reversed(processes):
            if process.poll() is None:
                process.kill()
                process.wait(timeout=10)


async def run(args, processes):
    runtime = args.runtime.resolve()
    sys.path.insert(0, str(runtime / "bench"))
    from signaling_stub import SignalingStub
    from playwright.async_api import async_playwright
    args.output.mkdir(parents=True)
    env = {"LD_LIBRARY_PATH": str(runtime / "lib")}

    def launch(name, command, cpus, extra_env=None):
        process = subprocess.Popen(["taskset", "-c", cpus, *map(str, command)], stdout=(args.output / f"{name}.log").open("w"),
                                   stderr=subprocess.STDOUT, env={**env, **(extra_env or {})})
        processes.append(process)
        return process

    class Blank(BaseHTTPRequestHandler):
        def do_GET(self):
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.end_headers()
            self.wfile.write(b"<!doctype html><title>t</title>")

        def log_message(self, *_):
            pass

    page_server = ThreadingHTTPServer(("127.0.0.1", 0), Blank)
    threading.Thread(target=page_server.serve_forever, daemon=True).start()
    result = {"server_bin": args.server_bin, "sources": {}}
    with SignalingStub() as stub:
        launch("server", [runtime / args.server_bin, "--threads", 2, "--bind-address", args.host, "--webrtc-address", args.host, "--rtmp-port", PORTS[0],
                          "--rtsp-port", PORTS[1], "--http-port", PORTS[2], "--control-token", "c-channels", "--signaling-url", stub.url],
               args.server_cpus, {"MEDIA_SERVER_LOG_LEVEL": "debug"})
        time.sleep(1)
        streams = {}
        for fixture in FIXTURES:
            name = fixture[2:-4]
            streams[name] = stub.issue("publish")
            (args.output / f"publish-{name}.urls").write_text(f"http://{args.host}:{PORTS[2]}/publish/whip/{streams[name]}\n")
            launch(f"publisher-{name}", [runtime / "build/whip_publisher", "--whip-urls-file", args.output / f"publish-{name}.urls", "--fixture",
                                         args.fixtures / fixture, "--sources", 1, "--bind-address", args.client_address, "--duration", 86400], args.client_cpus)
        time.sleep(4)
        viewers = []
        probes = []
        for name, stream in streams.items():
            for offer, fmtp in OFFERS.items():
                urls = args.output / f"{name}-{offer}.urls"
                urls.write_text(f"http://{args.host}:{PORTS[2]}/play/whep/{stream}/{stub.issue('play', stream)}\n")
                viewers.append((name, offer, launch(f"{name}-{offer}", [runtime / "build/whep_fanout", "--whep-urls-file", urls, "--viewers", 1,
                    "--ramp-per-second", 1, "--io-threads", 1, "--warmup", 1, "--duration", args.seconds, "--ready-timeout", 15, "--bind-address",
                    args.client_address, "--opus-fmtp", fmtp, "--dump-file", args.output / f"{name}-{offer}.dump"], args.client_cpus)))
            probes.append((name, launch(f"{name}-flv", [sys.executable, runtime / "bench/av_probe.py", "--seconds", args.seconds, "--ffmpeg-bin",
                                                         runtime / "bin/ffmpeg", f"http://{args.host}:{PORTS[2]}/{stream}/{stub.issue('play', stream)}.flv"],
                                        args.client_cpus)))
        chrome_stats = {name: [] for name in streams}
        async with async_playwright() as playwright:
            browser = await playwright.chromium.launch(executable_path=args.chrome_bin, args=["--allow-loopback-in-peer-connection",
                "--disable-features=WebRtcHideLocalIpsWithMdns", "--autoplay-policy=no-user-gesture-required", "--use-fake-ui-for-media-stream"])
            context = await browser.new_context()
            pages = {}
            for name, stream in streams.items():
                page = await context.new_page()
                await page.goto(f"http://localhost:{page_server.server_port}/")
                offer = await page.evaluate(PAGE_JS)
                request = urllib.request.Request(f"http://{args.host}:{PORTS[2]}/play/whep/{stream}/{stub.issue('play', stream)}", data=offer.encode(),
                                                 headers={"Content-Type": "application/sdp"}, method="POST")
                with urllib.request.urlopen(request, timeout=10) as response:
                    answer = response.read().decode()
                await page.evaluate("async ([sdp]) => pc.setRemoteDescription({type: 'answer', sdp})", [answer])
                pages[name] = page
                result["sources"].setdefault(name, {})["chrome_answer_opus"] = [line for line in answer.split("\r\n") if "opus" in line or "fmtp:111" in line]
            started = time.monotonic()
            while time.monotonic() - started < args.seconds:
                for name, page in pages.items():
                    sample = await page.evaluate(STATS_JS)
                    sample["t"] = round(time.monotonic() - started, 1)
                    chrome_stats[name].append(sample)
                await asyncio.sleep(2)
            await browser.close()
        for _, _, process in viewers:
            process.wait(timeout=args.seconds + 60)
        for _, process in probes:
            process.wait(timeout=args.seconds + 60)
    for process in reversed(processes):
        if process.poll() is None:
            process.kill()
            process.wait(timeout=10)
    log = (args.output / "server.log").read_text(errors="replace")
    result["egress_starts"] = [line.split("] ", 2)[-1] for line in log.splitlines() if "audio transcoder started input aac" in line]
    for name, offer, process in viewers:
        dump = args.output / f"{name}-{offer}.dump"
        replay = subprocess.run([sys.executable, runtime / "bench/whep_dump_av.py", dump, "--output", args.output / f"{name}-{offer}.mkv", "--ffmpeg-bin",
                                 runtime / "bin/ffmpeg"], capture_output=True, text=True, env=env)
        decoded = json.loads(replay.stdout.strip().splitlines()[-1]) if replay.stdout.strip() else {"error": replay.stderr[-300:]}
        result["sources"][name][offer] = {"exit": process.returncode, "toc": toc_buckets(dump),
                                          "decoded": {key: decoded.get(key) for key in ("audio_frames", "audio_pts_gaps", "audio_pts_overlaps", "decode_warnings",
                                                                                        "av_markers", "av_offset_ms_p50", "error")}}
    for name, process in probes:
        lines = (args.output / f"{name}-flv.log").read_text().splitlines()
        probe = json.loads(lines[-1]) if lines else {}
        result["sources"][name]["flv"] = {key: probe.get(key) for key in ("exit_code", "audio_frames", "audio_pts_gaps", "audio_pts_overlaps",
                                                                          "decode_warnings", "av_markers", "av_offset_ms_p50")}
    for name, samples in chrome_stats.items():
        last = samples[-1] if samples else {}
        first = next((sample for sample in samples if sample.get("totalSamplesReceived")), {})
        result["sources"][name]["chrome"] = {"samples": samples, "channels": last.get("channels"), "fmtp": last.get("fmtp"),
                                             "samples_received": (last.get("totalSamplesReceived") or 0) - (first.get("totalSamplesReceived") or 0),
                                             "concealed": (last.get("concealedSamples") or 0) - (first.get("concealedSamples") or 0),
                                             "concealment_events": (last.get("concealmentEvents") or 0) - (first.get("concealmentEvents") or 0),
                                             "packets_lost": last.get("packetsLost")}
    (args.output / "result.json").write_text(json.dumps(result, indent=1))
    for name, source in result["sources"].items():
        print(name, json.dumps({key: (value if key in ("flv", "chrome_answer_opus") else
                                      {k: v for k, v in value.items() if k != "samples"} if key == "chrome" else
                                      {"exit": value["exit"], "toc": value["toc"], **value["decoded"]}) for key, value in source.items()}))
    print("egress", json.dumps(result["egress_starts"]))


if __name__ == "__main__":
    main()
