#!/usr/bin/env python3
"""Play WHIP/RTMP sources of a debug-logging media_server in real Chromium and Firefox over WHEP, optionally publish from Chromium over WHIP,
and record each browser offer, the decoded audio/video stats and whether the server started an AAC->Opus egress for the viewer."""

import argparse
import asyncio
import json
import os
import shutil
import struct
import subprocess
import threading
import sys
import time
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

PAGE_JS = """async ([mode, degradation, play]) => {
  window.pc = new RTCPeerConnection();
  if (mode === 'whep') {
    pc.addTransceiver('video', {direction: 'recvonly'}); pc.addTransceiver('audio', {direction: 'recvonly'});
    if (play) pc.ontrack = e => { const el = document.createElement(e.track.kind); el.srcObject = new MediaStream([e.track]); el.autoplay = true;
                        document.body.appendChild(el); el.play().catch(() => {}); };
  } else {
    const s = await navigator.mediaDevices.getUserMedia({audio: true, video: {width: 1280, height: 720, frameRate: 30}});
    for (const t of s.getTracks()) {
      const sender = pc.addTransceiver(t, {direction: 'sendonly'}).sender;
      if (t.kind === 'video' && degradation) { const p = sender.getParameters(); p.degradationPreference = degradation; await sender.setParameters(p); }
    }
  }
  await pc.setLocalDescription(await pc.createOffer());
  await new Promise(r => { if (pc.iceGatheringState === 'complete') r(); pc.onicegatheringstatechange = () => pc.iceGatheringState === 'complete' && r(); setTimeout(r, 2000); });
  return pc.localDescription.sdp; }"""
STATS_JS = """async () => {
  const out = {connection: pc.connectionState, codecs: {}, inbound: {}, outbound: {}};
  (await pc.getStats()).forEach(s => {
    if (s.type === 'codec') out.codecs[s.id] = {mimeType: s.mimeType, channels: s.channels, sdpFmtpLine: s.sdpFmtpLine};
    if (s.type === 'inbound-rtp') out.inbound[s.kind] = {codecId: s.codecId, packetsReceived: s.packetsReceived, packetsLost: s.packetsLost,
      totalSamplesReceived: s.totalSamplesReceived, concealedSamples: s.concealedSamples, framesDecoded: s.framesDecoded, frameWidth: s.frameWidth};
    if (s.type === 'outbound-rtp') out.outbound[s.kind] = {codecId: s.codecId, packetsSent: s.packetsSent, framesEncoded: s.framesEncoded};
  });
  return out; }"""


TOGGLE_JS = """async () => {
  for (const sender of pc.getSenders().filter(s => s.track && s.track.kind === 'video')) {
    for (const active of [false, true]) { const p = sender.getParameters(); p.encodings.forEach(e => e.active = active); await sender.setParameters(p);
                                          await new Promise(r => setTimeout(r, 300)); }
  } }"""


def opus_toc(path):
    data = path.read_bytes() if path.exists() else b""
    counts = {"packets": 0, "stereo": 0, "configs": {}, "payload_bytes": 0}
    offset = 0
    while offset + 11 <= len(data):
        _, rtcp, size = struct.unpack_from("<qBH", data, offset)
        packet = data[offset + 11:offset + 11 + size]
        offset += 11 + size
        if rtcp or len(packet) < 13 or packet[1] & 0x7F != 111:
            continue
        header = 12 + 4 * (packet[0] & 0x0F)
        if packet[0] & 0x10:
            header += 4 + 4 * struct.unpack_from("!H", packet, header + 2)[0]
        if header >= len(packet):
            continue
        toc = packet[header]
        counts["packets"] += 1
        counts["stereo"] += bool(toc & 0x04)
        counts["configs"][toc >> 3] = counts["configs"].get(toc >> 3, 0) + 1
        counts["payload_bytes"] += len(packet) - header
    return counts


def post(url, body, content_type="application/sdp"):
    request = urllib.request.Request(url, data=body.encode(), headers={"Content-Type": content_type}, method="POST")
    with urllib.request.urlopen(request, timeout=10) as response:
        return response.status, response.read().decode()


def audio_fmtp(sdp):
    lines = sdp.split("\r\n")
    opus = [line.split(":", 1)[1].split()[0] for line in lines if line.startswith("a=rtpmap:") and "opus/48000" in line.lower()]
    return {"opus_rtpmap": [line for line in lines if line.startswith("a=rtpmap:") and "opus" in line.lower()],
            "opus_fmtp": [line for line in lines if any(line.startswith(f"a=fmtp:{pt} ") for pt in opus)],
            "ptime": [line for line in lines if "ptime" in line and not line.startswith("a=fmtp")]}


async def session(context, mode, url, seconds, on_connected=None):
    page = await context.new_page()
    await page.goto(page_url)
    offer = await page.evaluate(PAGE_JS, [mode, args.whip_degradation, context is not firefox_context or not args.firefox_no_play])
    status, answer = post(url, offer)
    await page.evaluate("async ([sdp]) => pc.setRemoteDescription({type: 'answer', sdp})", [answer])
    await page.wait_for_function("pc.connectionState === 'connected' || pc.connectionState === 'failed'", timeout=15000)
    if on_connected:
        await on_connected()
    await page.wait_for_timeout(seconds * 1000)
    stats = await page.evaluate(STATS_JS)
    return page, {"status": status, "offer": audio_fmtp(offer), "answer": audio_fmtp(answer), "offer_sdp": offer, "answer_sdp": answer, "stats": stats}


firefox_context = None


def log_size():
    return args.output.joinpath("server.log").stat().st_size


def egress_lines(offset):
    with args.output.joinpath("server.log").open() as log:
        log.seek(offset)
        return [line.strip() for line in log if "audio transcoder started input aac" in line]


async def main():
    sys.path.insert(0, str(args.runtime / "bench"))
    from playwright.async_api import async_playwright
    from signaling_stub import SignalingStub
    args.output.mkdir(parents=True)
    global page_url

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
    page_url = f"http://localhost:{page_server.server_port}/"
    host, client = args.host, args.client_address
    results = {"sessions": []}
    processes = []
    with SignalingStub() as stub:
        log = args.output.joinpath("server.log").open("w")
        processes.append(subprocess.Popen(["taskset", "-c", args.server_cpus, str(args.runtime / "build/media_server"), "--threads", "2", "--bind-address", host,
                                           "--webrtc-address", host, "--rtmp-port", "11935", "--rtsp-port", "18554", "--http-port", "18080",
                                           "--control-token", "browser-test", "--signaling-url", stub.url], stdout=log, stderr=subprocess.STDOUT,
                                          env={"MEDIA_SERVER_LOG_LEVEL": "debug", "LD_LIBRARY_PATH": str(args.runtime / "lib")}))
        time.sleep(1)
        sources = {}
        whip = stub.issue("publish")
        args.output.joinpath("whip.urls").write_text(f"http://{host}:18080/publish/whip/{whip}\n")
        processes.append(subprocess.Popen(["taskset", "-c", args.client_cpus, str(args.runtime / "build/whip_publisher"), "--whip-urls-file", str(args.output / "whip.urls"),
                                           "--fixture", str(args.runtime / ".cache/perf_fixture_whip.mkv"), "--sources", "1", "--bind-address", client,
                                           "--duration", "86400"], stdout=args.output.joinpath("whip_publisher.log").open("w"), stderr=subprocess.STDOUT,
                                          env={"LD_LIBRARY_PATH": str(args.runtime / "lib")}))
        sources["whip_publisher_stereo_opus"] = whip
        rtmp = stub.issue("publish")
        processes.append(subprocess.Popen(["taskset", "-c", args.client_cpus, str(args.runtime / "bin/ffmpeg"), "-nostdin", "-loglevel", "error", "-re", "-stream_loop", "-1",
                                           "-i", str(args.runtime / ".cache/perf_fixture_720p30.flv"), "-c", "copy", "-f", "flv", f"rtmp://{host}:11935/live/{rtmp}"],
                                          env={"LD_LIBRARY_PATH": str(args.runtime / "lib")}))
        sources["rtmp_aac"] = rtmp
        time.sleep(4)
        async with async_playwright() as playwright:
            chromium_browser = await playwright.chromium.launch(executable_path=args.chrome_bin, args=["--allow-loopback-in-peer-connection", "--use-fake-device-for-media-stream",
                                                               "--use-fake-ui-for-media-stream", "--disable-features=WebRtcHideLocalIpsWithMdns",
                                                               "--autoplay-policy=no-user-gesture-required"])
            chromium = await chromium_browser.new_context()
            profile = args.output / "firefox-profile"
            shutil.copytree(args.firefox_profile, profile)
            firefox = await playwright.firefox.launch_persistent_context(profile, env={**os.environ, "MOZ_DISABLE_GMP_SANDBOX": "1", "MOZ_DISABLE_CONTENT_SANDBOX": "1", "MOZ_DISABLE_RDD_SANDBOX": "1", "MOZ_DISABLE_SOCKET_PROCESS_SANDBOX": "1"}, firefox_user_prefs={
                "media.peerconnection.ice.loopback": True, "media.peerconnection.ice.obfuscate_host_addresses": False, "media.navigator.streams.fake": True,
                "media.navigator.permission.disabled": True, "media.autoplay.default": 0, "media.gmp-gmpopenh264.enabled": True,
                "media.gmp-gmpopenh264.visible": True, "media.gmp-gmpopenh264.version": args.openh264_version, "media.gmp-manager.updateEnabled": False})
            global firefox_context
            firefox_context = firefox
            agents = {}
            for name, context in (("chromium", chromium), ("firefox", firefox)):
                page = await context.new_page()
                agents[name] = await page.evaluate("navigator.userAgent")
                await page.close()
            results["browsers"] = agents
            publishers = []
            if args.browser_whip:
                stream = stub.issue("publish")
                offset = log_size()
                try:
                    page, item = await session(chromium, "whip", f"http://{host}:18080/publish/whip/{stream}", 3)
                    publishers.append(page)
                    sources["chromium_whip"] = stream
                except Exception as error:
                    item = {"error": str(error)[:300]}
                item.update(source="chromium_whip", browser="chromium", mode="whip", log=egress_lines(offset))
                results["sessions"].append(item)

            async def request_key_frame():
                await publishers[0].wait_for_timeout(500)
                await publishers[0].evaluate(TOGGLE_JS)

            if publishers:
                stream = sources["chromium_whip"]
                args.output.joinpath("dump.urls").write_text(f"http://{host}:18080/play/whep/{stream}/{stub.issue('play', stream)}\n")
                start = args.output / "dump.start"
                dump = subprocess.Popen([str(args.runtime / "build/whep_fanout"), "--whep-urls-file", str(args.output / "dump.urls"), "--viewers", "1",
                                         "--ramp-per-second", "1", "--io-threads", "1", "--warmup", "0", "--duration", "10", "--ready-timeout", "20",
                                         "--start-file", str(start), "--bind-address", client, "--dump-file", str(args.output / "chromium_whip.dump")],
                                        stdout=args.output.joinpath("dump.log").open("w"), stderr=subprocess.STDOUT, env={"LD_LIBRARY_PATH": str(args.runtime / "lib")})
                await asyncio.sleep(3)
                await request_key_frame()
                for _ in range(100):
                    if "phase=media_ready" in args.output.joinpath("dump.log").read_text():
                        break
                    await asyncio.sleep(0.1)
                start.write_text(f"{time.monotonic_ns() + 500000000}\n")
                await asyncio.sleep(12)
                dump.wait(timeout=30)
                results["chromium_whip_opus_toc"] = opus_toc(args.output / "chromium_whip.dump")
                print("chromium_whip opus toc", results["chromium_whip_opus_toc"], flush=True)
            for source, stream in sources.items():
                for name, browser in (("chromium", chromium), ("firefox", firefox)):
                    offset = log_size()
                    try:
                        page, item = await session(browser, "whep", f"http://{host}:18080/play/whep/{stream}/{stub.issue('play', stream)}", args.seconds,
                                                   request_key_frame if source == "chromium_whip" else None)
                        await page.close()
                    except Exception as error:
                        item = {"error": str(error)[:300]}
                    time.sleep(1)
                    item.update(source=source, browser=name, mode="whep", egress_started=egress_lines(offset))
                    results["sessions"].append(item)
                    args.output.joinpath("result.json").write_text(json.dumps(results, indent=1))
                    print(source, name, item.get("offer", {}).get("opus_fmtp"), "egress" if item.get("egress_started") else "passthrough/none",
                          json.dumps(item.get("stats", {}).get("inbound", {})), item.get("error", ""), flush=True)
            for page in publishers:
                await page.close()
            await chromium_browser.close()
            await firefox.close()
        for process in reversed(processes):
            process.kill()
            process.wait(timeout=10)
    args.output.joinpath("result.json").write_text(json.dumps(results, indent=1))


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--output", type=Path, required=True)
parser.add_argument("--runtime", type=Path, required=True)
parser.add_argument("--host", default="198.19.0.10")
parser.add_argument("--client-address", default="198.19.0.11")
parser.add_argument("--server-cpus", default="0-1")
parser.add_argument("--client-cpus", default="8-15")
parser.add_argument("--seconds", type=int, default=10)
parser.add_argument("--browser-whip", action="store_true")
parser.add_argument("--chrome-bin", type=Path, required=True, help="Google Chrome executable; Playwright's Chromium has no H.264")
parser.add_argument("--firefox-profile", type=Path, required=True, help="profile template holding gmp-gmpopenh264/<version>/")
parser.add_argument("--openh264-version", default="2.6.0")
parser.add_argument("--firefox-no-play", action="store_true", help="do not attach Firefox tracks to media elements; its content process crashes on playback without a sound card")
parser.add_argument("--whip-degradation", default="", help="degradationPreference of the Chrome WHIP video sender; empty keeps the browser default")
args = parser.parse_args()
asyncio.run(main())
