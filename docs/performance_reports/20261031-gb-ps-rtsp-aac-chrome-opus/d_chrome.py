#!/usr/bin/env python3
"""Stage D browser check of Opus passthrough. Three WHIP sources: whip_publisher with a mono fixture, whip_publisher with a stereo fixture whose
right channel is the left at -12 dB, and a real Chrome publisher (fake camera video kept at one resolution because WHIP rejects video config changes, WebAudio beep with the right channel at -12 dB) that renegotiates stereo=1 and back to
stereo=0 locally (its video encoding is paused and resumed to force a key frame once viewers are attached) against the unchanged server answer. Each source is watched by whep_fanout viewers with several Opus fmtp lines (TOC stereo
flags and replayed decode) and by real Chrome viewers whose WebAudio channel splitter measures the right/left RMS ratio actually played:
about 0.25 means stereo playback, 0 or 1 means mono. One source also gets Chrome offers carrying maxaveragebitrate, maxplaybackrate and
a=maxptime limits. An HTTP-FLV reader joins half way to start the AAC path, then the sources end (killed, terminated, WHIP DELETE) and the
script records how viewers and the server clean up."""

import argparse
import asyncio
import json
import signal
import statistics
import struct
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

PORTS = (11935, 18554, 18080)
TOKEN = "d-chrome"
FANOUT_OFFERS = {"stereo0": "minptime=10;useinbandfec=1;stereo=0", "chrome": "minptime=10;useinbandfec=1",
                 "firefox": "maxplaybackrate=48000;stereo=1;useinbandfec=1", "bitrate32k": "minptime=10;useinbandfec=1;maxaveragebitrate=32000",
                 "rate16k": "minptime=10;useinbandfec=1;maxplaybackrate=16000"}
CHROME_LIMITS = {"default": None, "bitrate32k": ";maxaveragebitrate=32000", "rate16k": ";maxplaybackrate=16000", "maxptime60": "maxptime"}
BUCKET_SECONDS = 10
GATHER_JS = """await new Promise(r => { if (pc.iceGatheringState === 'complete') r(); pc.onicegatheringstatechange = () =>
  pc.iceGatheringState === 'complete' && r(); setTimeout(r, 2000); });"""
VIEWER_JS = """async () => {
  window.pc = new RTCPeerConnection();
  window.ctx = new AudioContext(); await ctx.resume();
  window.levels = {left: [], right: [], ratios: []};
  pc.addTransceiver('video', {direction: 'recvonly'}); pc.addTransceiver('audio', {direction: 'recvonly'});
  pc.ontrack = e => {
    const el = document.createElement(e.track.kind); el.srcObject = new MediaStream([e.track]); el.autoplay = true; el.muted = true;
    document.body.appendChild(el); el.play().catch(() => {});
    if (e.track.kind !== 'audio') return;
    const split = ctx.createChannelSplitter(2);
    ctx.createMediaStreamSource(new MediaStream([e.track])).connect(split);
    const meters = [0, 1].map(i => { const a = ctx.createAnalyser(); a.fftSize = 2048; split.connect(a, i); return a; });
    const data = new Float32Array(2048);
    setInterval(() => {
      const rms = meters.map(a => { a.getFloatTimeDomainData(data); let s = 0; for (const x of data) s += x * x; return Math.sqrt(s / data.length); });
      levels.left.push(rms[0]); levels.right.push(rms[1]); if (rms[0] > 0.01) levels.ratios.push(rms[1] / rms[0]);
    }, 50);
  };
  await pc.setLocalDescription(await pc.createOffer());
  """ + GATHER_JS + """
  return pc.localDescription.sdp; }"""
PUBLISHER_JS = """async () => {
  window.ctx = new AudioContext({sampleRate: 48000}); await ctx.resume();
  const osc = ctx.createOscillator(); osc.frequency.value = 1000; const gate = ctx.createGain(); gate.gain.value = 0; osc.connect(gate); osc.start();
  const t0 = ctx.currentTime + 0.5;
  for (let i = 0; i < 900; i++) { gate.gain.setValueAtTime(0.5, t0 + i); gate.gain.setValueAtTime(0, t0 + i + 0.1); }
  const merger = ctx.createChannelMerger(2); const right = ctx.createGain(); right.gain.value = 0.25;
  gate.connect(merger, 0, 0); gate.connect(right); right.connect(merger, 0, 1);
  const destination = ctx.createMediaStreamDestination(); destination.channelCount = 2; merger.connect(destination);
  const camera = await navigator.mediaDevices.getUserMedia({video: {width: 640, height: 360, frameRate: 30}});
  window.pc = new RTCPeerConnection();
  window.video = pc.addTransceiver(camera.getVideoTracks()[0], {direction: 'sendonly'}).sender;
  const parameters = video.getParameters(); parameters.degradationPreference = 'maintain-resolution'; await video.setParameters(parameters);
  pc.addTransceiver(destination.stream.getAudioTracks()[0], {direction: 'sendonly'});
  await pc.setLocalDescription(await pc.createOffer());
  """ + GATHER_JS + """
  return pc.localDescription.sdp; }"""
KEY_FRAME_JS = """async () => {
  for (const active of [false, true]) { const p = video.getParameters(); p.encodings.forEach(e => e.active = active); await video.setParameters(p);
                                        await new Promise(r => setTimeout(r, 300)); } }"""
SWITCH_JS = """async ([stereo]) => {
  await pc.setLocalDescription(await pc.createOffer());
  const sdp = window.answer.replace(/(a=rtpmap:(\\d+) opus\\/48000\\/2\\r\\n)/i,
    (line, _, pt) => line + `a=fmtp:${pt} minptime=10;useinbandfec=1;stereo=${stereo}\\r\\n`);
  await pc.setRemoteDescription({type: 'answer', sdp});
  return sdp.split('\\r\\n').filter(l => l.startsWith('a=fmtp:') && l.includes('stereo')); }"""
STATS_JS = """async ([kind]) => {
  const out = {connection: pc.connectionState};
  const stats = await pc.getStats();
  stats.forEach(s => {
    if (s.type === kind && s.kind === 'audio') Object.assign(out, {codecId: s.codecId, packets: s.packetsReceived ?? s.packetsSent,
      packetsLost: s.packetsLost, totalSamplesReceived: s.totalSamplesReceived, concealedSamples: s.concealedSamples,
      concealmentEvents: s.concealmentEvents, audioLevel: s.audioLevel, bytes: s.bytesReceived ?? s.bytesSent});
  });
  stats.forEach(s => { if (s.type === 'codec' && s.id === out.codecId) Object.assign(out, {channels: s.channels, fmtp: s.sdpFmtpLine}); });
  if (window.ctx) out.audio_context = ctx.state;
  if (window.levels) {
    const mean = v => v.length ? v.reduce((a, b) => a + b, 0) / v.length : null;
    const sorted = [...levels.ratios].sort((a, b) => a - b);
    Object.assign(out, {left_rms: mean(levels.left), right_rms: mean(levels.right), ratio_p50: sorted.length ? sorted[sorted.length >> 1] : null,
                        loud_windows: sorted.length});
    levels.left = []; levels.right = []; levels.ratios = [];
  }
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


def munge_offer(sdp, limit):
    if limit is None:
        return sdp
    lines = sdp.split("\r\n")
    for index, line in enumerate(lines):
        if line.lower().startswith("a=rtpmap:111 opus/48000/2"):
            if limit == "maxptime":
                lines.insert(index + 1, "a=maxptime:60")
            break
    if limit != "maxptime":
        lines = [line + limit if line.startswith("a=fmtp:111 ") else line for line in lines]
    return "\r\n".join(lines)


def post_sdp(url, sdp):
    request = urllib.request.Request(url, data=sdp.encode(), headers={"Content-Type": "application/sdp"}, method="POST")
    with urllib.request.urlopen(request, timeout=10) as response:
        return response.read().decode(), response.headers.get("Location")


def opus_lines(sdp):
    return [line for line in sdp.split("\r\n") if "opus" in line.lower() or line.startswith("a=fmtp:111") or line.startswith("a=maxptime")]


def window_summary(samples, start, end):
    selected = [sample for sample in samples if start <= sample["t"] < end and sample.get("ratio_p50") is not None]
    ratios = [sample["ratio_p50"] for sample in selected]
    return {"windows": len(selected), "ratio_p50": round(statistics.median(ratios), 3) if ratios else None,
            "ratio_min": round(min(ratios), 3) if ratios else None, "ratio_max": round(max(ratios), 3) if ratios else None,
            "left_rms": round(statistics.median(sample["left_rms"] for sample in selected), 4) if selected else None,
            "channels": sorted({sample.get("channels") for sample in selected if sample.get("channels") is not None})}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--server-bin", required=True)
    parser.add_argument("--fixtures", type=Path, required=True, help="directory with d-mono.mkv and d-lr.mkv")
    parser.add_argument("--chrome-bin", type=Path, required=True)
    parser.add_argument("--seconds", type=int, default=90)
    parser.add_argument("--host", default="198.19.0.10")
    parser.add_argument("--client-address", default="198.19.0.11")
    parser.add_argument("--server-cpus", default="0-3")
    parser.add_argument("--client-cpus", default="8-15")
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
    from perf_run import request
    from playwright.async_api import async_playwright
    args.output.mkdir(parents=True)
    env = {"LD_LIBRARY_PATH": str(runtime / "lib")}
    base = f"http://{args.host}:{PORTS[2]}"

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
    result = {"server_bin": args.server_bin, "sources": {}, "timeline": {}}
    seconds = args.seconds
    with SignalingStub() as stub:
        server = launch("server", [runtime / args.server_bin, "--threads", 2, "--bind-address", args.host, "--webrtc-address", args.host, "--rtmp-port",
                                   PORTS[0], "--rtsp-port", PORTS[1], "--http-port", PORTS[2], "--control-token", TOKEN, "--signaling-url", stub.url],
                        args.server_cpus, {"MEDIA_SERVER_LOG_LEVEL": "debug"})
        time.sleep(1)
        streams = {name: stub.issue("publish") for name in ("mono", "lr", "chrome_pub")}
        publishers = {}
        for name, fixture in (("mono", "d-mono.mkv"), ("lr", "d-lr.mkv")):
            (args.output / f"publish-{name}.urls").write_text(f"{base}/publish/whip/{streams[name]}\n")
            publishers[name] = launch(f"publisher-{name}", [runtime / "build/whip_publisher", "--whip-urls-file", args.output / f"publish-{name}.urls",
                                                            "--fixture", args.fixtures / fixture, "--sources", 1, "--bind-address", args.client_address,
                                                            "--duration", 86400], args.client_cpus)
        async with async_playwright() as playwright:
            browser = await playwright.chromium.launch(executable_path=args.chrome_bin, args=["--allow-loopback-in-peer-connection",
                "--disable-features=WebRtcHideLocalIpsWithMdns", "--autoplay-policy=no-user-gesture-required", "--use-fake-device-for-media-stream",
                "--use-fake-ui-for-media-stream"])
            context = await browser.new_context()
            publisher_page = await context.new_page()
            await publisher_page.goto(f"http://localhost:{page_server.server_port}/")
            offer = await publisher_page.evaluate(PUBLISHER_JS)
            answer, publisher_location = post_sdp(f"{base}/publish/whip/{streams['chrome_pub']}", offer)
            await publisher_page.evaluate("async ([sdp]) => { window.answer = sdp; await pc.setRemoteDescription({type: 'answer', sdp}); }", [answer])
            result["chrome_publisher"] = {"offer_opus": opus_lines(offer), "answer_opus": opus_lines(answer), "location": publisher_location,
                                          "switches": [], "samples": []}
            receivers = set()
            for _ in range(100):
                receivers = {item["stream_id"] for item in request(base, "/receivers", token=TOKEN)["receivers"] if item["ready"]}
                if set(streams.values()) <= receivers:
                    break
                await asyncio.sleep(0.2)
            result["sources_registered"] = sorted(name for name, stream in streams.items() if stream in receivers)
            viewers = []
            for name, stream in streams.items():
                for offer_name, fmtp in FANOUT_OFFERS.items():
                    urls = args.output / f"{name}-{offer_name}.urls"
                    urls.write_text(f"{base}/play/whep/{stream}/{stub.issue('play', stream)}\n")
                    viewers.append((name, offer_name, launch(f"{name}-{offer_name}", [runtime / "build/whep_fanout", "--whep-urls-file", urls, "--viewers", 1,
                        "--ramp-per-second", 1, "--io-threads", 1, "--warmup", 1, "--duration", seconds, "--ready-timeout", 15, "--bind-address",
                        args.client_address, "--opus-fmtp", fmtp, "--dump-file", args.output / f"{name}-{offer_name}.dump"], args.client_cpus)))
            await asyncio.sleep(3)
            await publisher_page.evaluate(KEY_FRAME_JS)
            pages = {}
            for name, stream in streams.items():
                for limit_name, limit in CHROME_LIMITS.items():
                    if limit is not None and name != "lr":
                        continue
                    page = await context.new_page()
                    await page.goto(f"http://localhost:{page_server.server_port}/")
                    offer = munge_offer(await page.evaluate(VIEWER_JS), limit)
                    answer, _ = post_sdp(f"{base}/play/whep/{stream}/{stub.issue('play', stream)}", offer)
                    await page.evaluate("async ([sdp]) => pc.setRemoteDescription({type: 'answer', sdp})", [answer])
                    pages[(name, limit_name)] = page
                    result["sources"].setdefault(name, {}).setdefault("browser", {})[limit_name] = {"offer_opus": opus_lines(offer),
                                                                                                   "answer_opus": opus_lines(answer), "samples": []}
            await publisher_page.evaluate(KEY_FRAME_JS)
            started = time.monotonic()
            switch_at = {round(seconds / 3): 1, round(2 * seconds / 3): 0}
            flv = {}
            while time.monotonic() - started < seconds:
                elapsed = time.monotonic() - started
                for at, stereo in list(switch_at.items()):
                    if elapsed >= at:
                        lines = await publisher_page.evaluate(SWITCH_JS, [stereo])
                        result["chrome_publisher"]["switches"].append({"t": round(elapsed, 1), "stereo": stereo, "answer_fmtp": lines})
                        del switch_at[at]
                if not flv and elapsed >= seconds / 2:
                    result["timeline"]["flv_join"] = round(elapsed, 1)
                    for name in ("lr", "chrome_pub"):
                        flv[name] = launch(f"{name}-flv", [sys.executable, runtime / "bench/av_probe.py", "--seconds", int(seconds / 2) - 5, "--ffmpeg-bin",
                                                           runtime / "bin/ffmpeg", f"{base}/{streams[name]}/{stub.issue('play', streams[name])}.flv"],
                                           args.client_cpus)
                for (name, limit_name), page in pages.items():
                    sample = await page.evaluate(STATS_JS, ["inbound-rtp"])
                    sample["t"] = round(elapsed, 1)
                    result["sources"][name]["browser"][limit_name]["samples"].append(sample)
                sample = await publisher_page.evaluate(STATS_JS, ["outbound-rtp"])
                sample["t"] = round(elapsed, 1)
                result["chrome_publisher"]["samples"].append(sample)
                await asyncio.sleep(2)
            for _, _, process in viewers:
                process.wait(timeout=60)
            for process in flv.values():
                process.wait(timeout=seconds)
            end = time.monotonic()
            result["timeline"]["source_end"] = round(end - started, 1)
            publishers["mono"].send_signal(signal.SIGKILL)
            publishers["lr"].send_signal(signal.SIGTERM)
            delete = urllib.request.Request(f"{base}{publisher_location}", method="DELETE")
            try:
                with urllib.request.urlopen(delete, timeout=10) as response:
                    result["chrome_publisher"]["delete_status"] = response.status
            except urllib.error.HTTPError as error:
                result["chrome_publisher"]["delete_status"] = error.code
            await publisher_page.evaluate("() => pc.close()")
            remaining = []
            for _ in range(60):
                remaining = sorted(name for name, stream in streams.items()
                                   if stream in {item["stream_id"] for item in request(base, "/receivers", token=TOKEN)["receivers"]})
                if not remaining:
                    break
                await asyncio.sleep(1)
            result["cleanup"] = {"receivers_remaining": remaining, "receivers_gone_after_s": round(time.monotonic() - end, 1)}
            await asyncio.sleep(5)
            result["cleanup"]["chrome_viewer_states"] = {f"{name}/{limit}": await page.evaluate("() => pc.connectionState")
                                                         for (name, limit), page in pages.items()}
            await browser.close()
        await asyncio.sleep(3)
        result["cleanup"]["server_alive"] = server.poll() is None
    for process in reversed(processes):
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=10)
    log = (args.output / "server.log").read_text(errors="replace")
    count = lambda text: sum(text in line for line in log.splitlines())
    result["cleanup"].update({"whep_started": count("webrtc whep session started"), "whep_shutdown": count("webrtc whep session shutdown"),
                              "whep_source_ended": count("webrtc source stream ended"), "whip_started": count("webrtc whip session started"),
                              "whip_shutdown": count("webrtc whip session shutdown")})
    result["egress_starts"] = [line.split("] ", 2)[-1] for line in log.splitlines() if "audio transcoder started input aac" in line]
    result["opus_aac_starts"] = count("audio transcoder started input opus")
    for name, offer_name, process in viewers:
        dump = args.output / f"{name}-{offer_name}.dump"
        replay = subprocess.run([sys.executable, runtime / "bench/whep_dump_av.py", dump, "--output", args.output / f"{name}-{offer_name}.mkv",
                                 "--ffmpeg-bin", runtime / "bin/ffmpeg"], capture_output=True, text=True, env=env)
        decoded = json.loads(replay.stdout.strip().splitlines()[-1]) if replay.stdout.strip() else {"error": replay.stderr[-300:]}
        result["sources"][name][offer_name] = {"exit": process.returncode, "toc": toc_buckets(dump),
                                               "decoded": {key: decoded.get(key) for key in ("audio_frames", "audio_pts_gaps", "audio_pts_overlaps",
                                                                                             "decode_warnings", "av_markers", "av_offset_ms_p50", "error")}}
    for name in flv:
        lines = (args.output / f"{name}-flv.log").read_text().splitlines()
        probe = json.loads(lines[-1]) if lines else {}
        result["sources"][name]["flv_join"] = {key: probe.get(key) for key in ("exit_code", "audio_frames", "audio_pts_gaps", "audio_pts_overlaps",
                                                                               "decode_warnings", "av_markers", "av_offset_ms_p50")}
    switches = [switch["t"] for switch in result["chrome_publisher"]["switches"]]
    phases = list(zip([0] + switches, switches + [seconds]))
    for name, source in result["sources"].items():
        for limit_name, chrome in source["browser"].items():
            samples = chrome["samples"]
            first = next((sample for sample in samples if sample.get("totalSamplesReceived")), {})
            last = samples[-1] if samples else {}
            chrome["summary"] = {"fmtp": last.get("fmtp"), "channels": last.get("channels"),
                                 "samples_received": (last.get("totalSamplesReceived") or 0) - (first.get("totalSamplesReceived") or 0),
                                 "concealed": (last.get("concealedSamples") or 0) - (first.get("concealedSamples") or 0),
                                 "concealment_events": (last.get("concealmentEvents") or 0) - (first.get("concealmentEvents") or 0),
                                 "packets_lost": last.get("packetsLost"),
                                 "playback": [window_summary(samples, start + 4, end) for start, end in phases] if name == "chrome_pub"
                                 else [window_summary(samples, 4, seconds)]}
    (args.output / "result.json").write_text(json.dumps(result, indent=1))
    print("registered", result["sources_registered"], "publisher", json.dumps({k: v for k, v in result["chrome_publisher"].items() if k != "samples"}))
    for name, source in result["sources"].items():
        for key, value in source.items():
            if key == "browser":
                for limit_name, chrome in value.items():
                    print(name, "chrome", limit_name, json.dumps({"answer_opus": chrome["answer_opus"], **chrome["summary"]}))
            else:
                print(name, key, json.dumps(value))
    print("egress", len(result["egress_starts"]), json.dumps(result["egress_starts"]), "opus_aac_starts", result["opus_aac_starts"])
    print("cleanup", json.dumps(result["cleanup"]))


if __name__ == "__main__":
    main()
