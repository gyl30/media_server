#!/usr/bin/env python3
"""WHEP negotiation and lifecycle boundary check. The server under test gets two Opus sources: a WHIP publisher and an active WHEP pull from
a second media_server that itself receives a WHIP publisher. Each source is watched by real Chrome viewers (video-only recvonly offer,
video plus audio restricted to PCMU, default video plus audio) and by whep_fanout viewers (--video-only, --unsupported-audio, default).
An HTTP-FLV reader joins in the middle third to start Opus->AAC on demand. One Chrome video-only viewer is deleted while the source keeps
running, the native viewers delete their sessions when they finish, and the remaining viewers stay connected while the sources stop
(WHIP DELETE for the direct source, upstream publisher terminated for the pull). Opus->AAC and AAC->Opus starts are counted from the
server's debug log at each phase boundary; sessions, receivers, FD and PSS are checked after cleanup."""

import argparse
import asyncio
import json
import os
import re
import signal
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

PORTS = (11935, 18554, 18080)
UPSTREAM_PORTS = (21935, 28554, 28080)
TOKEN = "whep-boundary"
MODES = ("video", "pcmu", "av")
FANOUT_MODES = {"video": ["--video-only"], "g722": ["--unsupported-audio"], "av": []}
VIEWER_JS = """async ([mode]) => {
  window.pc = new RTCPeerConnection();
  pc.addTransceiver('video', {direction: 'recvonly'});
  if (mode !== 'video') {
    const audio = pc.addTransceiver('audio', {direction: 'recvonly'});
    if (mode === 'pcmu') audio.setCodecPreferences(RTCRtpReceiver.getCapabilities('audio').codecs.filter(c => c.mimeType === 'audio/PCMU'));
  }
  pc.ontrack = e => {
    const el = document.createElement(e.track.kind); el.srcObject = new MediaStream([e.track]); el.autoplay = true; el.muted = true;
    document.body.appendChild(el); el.play().catch(() => {});
    if (e.track.kind === 'video') window.videoElement = el;
  };
  await pc.setLocalDescription(await pc.createOffer());
  await new Promise(r => { if (pc.iceGatheringState === 'complete') r(); pc.onicegatheringstatechange = () =>
    pc.iceGatheringState === 'complete' && r(); setTimeout(r, 2000); });
  return pc.localDescription.sdp; }"""
STATS_JS = """async () => {
  const out = {connection: pc.connectionState,
               transceivers: pc.getTransceivers().map(t => [t.mid, t.receiver.track.kind, t.currentDirection])};
  (await pc.getStats()).forEach(s => {
    if (s.type !== 'inbound-rtp') return;
    if (s.kind === 'video') Object.assign(out, {video_packets: s.packetsReceived, video_lost: s.packetsLost, frames_decoded: s.framesDecoded,
      frames_dropped: s.framesDropped, freeze_count: s.freezeCount, freeze_seconds: s.totalFreezesDuration, width: s.frameWidth,
      height: s.frameHeight, fps: s.framesPerSecond});
    if (s.kind === 'audio') Object.assign(out, {audio_packets: s.packetsReceived, audio_concealed: s.concealedSamples});
  });
  out.video_time = window.videoElement ? videoElement.currentTime : null;
  return out; }"""


def media_lines(sdp):
    return [line for line in sdp.split("\r\n") if line.startswith("m=") or line.startswith("a=rtpmap") or line in ("a=inactive", "a=sendonly",
                                                                                                                     "a=recvonly", "a=sendrecv")]


def http(method, url, body=None, token=None):
    headers = {"Content-Type": "application/sdp" if isinstance(body, str) else "application/json"}
    if token:
        headers["Authorization"] = f"Bearer {token}"
    data = body.encode() if isinstance(body, str) else json.dumps(body).encode() if body is not None else None
    request = urllib.request.Request(url, data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(request, timeout=10) as response:
            return response.status, response.read().decode(), response.headers.get("Location")
    except urllib.error.HTTPError as error:
        return error.code, error.read().decode(errors="replace"), None


def pss_kib(pid):
    return sum(int(line.split()[1]) for line in Path(f"/proc/{pid}/smaps_rollup").read_text().splitlines() if line.startswith("Pss:"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--server-bin", required=True)
    parser.add_argument("--chrome-bin", type=Path, required=True)
    parser.add_argument("--seconds", type=int, default=60)
    parser.add_argument("--workers", type=int, default=4)
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
    from playwright.async_api import async_playwright
    args.output.mkdir(parents=True)
    env = {"LD_LIBRARY_PATH": str(runtime / "lib")}
    base = f"http://{args.host}:{PORTS[2]}"
    upstream_base = f"http://{args.client_address}:{UPSTREAM_PORTS[2]}"

    def launch(name, command, cpus, extra_env=None):
        process = subprocess.Popen(["taskset", "-c", cpus, *map(str, command)], stdout=(args.output / f"{name}.log").open("w"),
                                   stderr=subprocess.STDOUT, env={**os.environ, **env, **(extra_env or {})})
        processes.append(process)
        return process

    def receivers(url):
        return {item["stream_id"]: item for item in json.loads(http("GET", f"{url}/receivers", token=TOKEN)[1])["receivers"]}

    def server_counts():
        log = (args.output / "server.log").read_text(errors="replace")
        count = lambda pattern: len(re.findall(pattern, log))
        return {"opus_aac_starts": count(r"audio transcoder started input opus"), "aac_opus_starts": count(r"audio transcoder started input aac"),
                "whep_started": count(r"webrtc whep session started"), "whep_shutdown": count(r"webrtc whep session shutdown"),
                "whep_source_ended": count(r"webrtc source stream ended"), "fd": len(list(Path(f"/proc/{server.pid}/fd").iterdir())),
                "pss_mib": round(pss_kib(server.pid) / 1024, 1)}

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
    seconds = args.seconds
    result = {"server_bin": args.server_bin, "phases": {}, "chrome": {}, "fanout": {}, "flv": {}}
    with SignalingStub() as stub:
        server = launch("server", [runtime / args.server_bin, "--threads", args.workers, "--bind-address", args.host, "--webrtc-address", args.host,
                                   "--rtmp-port", PORTS[0], "--rtsp-port", PORTS[1], "--http-port", PORTS[2], "--control-token", TOKEN,
                                   "--signaling-url", stub.url], args.server_cpus, {"MEDIA_SERVER_LOG_LEVEL": "debug"})
        launch("upstream", [runtime / "build/media_server", "--threads", 2, "--bind-address", args.client_address, "--webrtc-address",
                            args.client_address, "--rtmp-port", UPSTREAM_PORTS[0], "--rtsp-port", UPSTREAM_PORTS[1], "--http-port",
                            UPSTREAM_PORTS[2], "--control-token", TOKEN, "--signaling-url", stub.url], args.client_cpus)
        time.sleep(1)
        result["phases"]["server_started"] = server_counts()
        whip_stream, upstream_stream, pull_stream = stub.issue("publish", ttl=600), stub.issue("publish", ttl=600), str(uuid.uuid4())
        publishers = {}
        for name, url in (("whip", f"{base}/publish/whip/{whip_stream}"), ("upstream", f"{upstream_base}/publish/whip/{upstream_stream}")):
            (args.output / f"publish-{name}.urls").write_text(url + "\n")
            publishers[name] = launch(f"publisher-{name}", [runtime / "build/whip_publisher", "--whip-urls-file", args.output / f"publish-{name}.urls",
                                                            "--fixture", runtime / ".cache/perf_fixture_whip.mkv", "--sources", 1, "--bind-address",
                                                            args.client_address, "--duration", 86400], args.client_cpus)
        for _ in range(100):
            if upstream_stream in receivers(upstream_base):
                break
            time.sleep(0.2)
        status, body, _ = http("POST", f"{base}/pull/create", {"stream_id": pull_stream,
                                                                "url": f"{upstream_base}/play/whep/{upstream_stream}/{stub.issue('play', upstream_stream, ttl=600)}"}, TOKEN)
        result["pull_create"] = {"status": status, "body": body[:200]}
        streams = {"whip": whip_stream, "pull": pull_stream}
        ready = {}
        for _ in range(100):
            ready = {name for name, stream in streams.items() if receivers(base).get(stream, {}).get("ready")}
            if ready == set(streams):
                break
            time.sleep(0.2)
        result["sources_ready"] = sorted(ready)
        if ready != set(streams):
            (args.output / "result.json").write_text(json.dumps(result, indent=1))
            print(json.dumps(result))
            return
        await asyncio.sleep(3)
        result["phases"]["sources_ready"] = server_counts()
        fanouts = {}
        for name, stream in streams.items():
            for mode, extra in FANOUT_MODES.items():
                urls = args.output / f"fanout-{name}-{mode}.urls"
                urls.write_text(f"{base}/play/whep/{stream}/{stub.issue('play', stream, ttl=600)}\n")
                fanouts[(name, mode)] = launch(f"fanout-{name}-{mode}", [runtime / "build/whep_fanout", "--whep-urls-file", urls, "--viewers", 1,
                                                                         "--ramp-per-second", 1, "--io-threads", 1, "--warmup", 2, "--duration",
                                                                         seconds - 4, "--ready-timeout", 15, "--bind-address", args.client_address,
                                                                         *extra], args.client_cpus)
        async with async_playwright() as playwright:
            browser = await playwright.chromium.launch(executable_path=args.chrome_bin, args=["--allow-loopback-in-peer-connection",
                "--disable-features=WebRtcHideLocalIpsWithMdns", "--autoplay-policy=no-user-gesture-required"])
            context = await browser.new_context()
            pages = {}
            for name, stream in streams.items():
                for mode in MODES:
                    page = await context.new_page()
                    await page.goto(f"http://localhost:{page_server.server_port}/")
                    offer = await page.evaluate(VIEWER_JS, [mode])
                    status, answer, location = http("POST", f"{base}/play/whep/{stream}/{stub.issue('play', stream, ttl=600)}", offer)
                    entry = {"post_status": status, "offer": media_lines(offer), "answer": media_lines(answer), "location": location, "samples": []}
                    if status == 201:
                        await page.evaluate("async ([sdp]) => pc.setRemoteDescription({type: 'answer', sdp})", [answer])
                        pages[(name, mode)] = page
                    result["chrome"][f"{name}/{mode}"] = entry
            started = time.monotonic()
            flv = {}
            deleted = None
            while time.monotonic() - started < seconds:
                elapsed = time.monotonic() - started
                if not flv and elapsed >= seconds / 3:
                    result["phases"]["before_flv"] = server_counts()
                    for name, stream in streams.items():
                        flv[name] = launch(f"flv-{name}", [sys.executable, runtime / "bench/av_probe.py", "--seconds", int(seconds / 3) - 5, "--ffmpeg-bin",
                                                           runtime / "bin/ffmpeg", f"{base}/{stream}/{stub.issue('play', stream, ttl=600)}.flv"],
                                           args.client_cpus)
                if deleted is None and elapsed >= 2 * seconds / 3:
                    for process in flv.values():
                        process.wait(timeout=30)
                    result["phases"]["after_flv"] = server_counts()
                    entry = result["chrome"]["whip/video"]
                    before = server_counts()["whep_shutdown"]
                    status, _, _ = http("DELETE", f"{base}{entry['location']}")
                    await pages[("whip", "video")].evaluate("() => pc.close()")
                    deleted = pages.pop(("whip", "video"))
                    await asyncio.sleep(1)
                    entry["delete"] = {"status": status, "shutdown_within_1s": server_counts()["whep_shutdown"] - before}
                for (name, mode), page in pages.items():
                    sample = await page.evaluate(STATS_JS)
                    sample["t"] = round(elapsed, 1)
                    result["chrome"][f"{name}/{mode}"]["samples"].append(sample)
                await asyncio.sleep(2)
            for (name, mode), process in fanouts.items():
                process.wait(timeout=60)
                text = (args.output / f"fanout-{name}-{mode}.log").read_text()
                fields = {}
                for prefix in ("phase=measurement ", "phase=disconnect "):
                    lines = [line for line in text.splitlines() if line.startswith(prefix)]
                    fields.update(dict(re.findall(r"(\w+)=(\S+)", lines[-1])) if lines else {})
                result["fanout"][f"{name}/{mode}"] = {"exit": process.returncode, **{key: fields.get(key) for key in (
                    "ready", "progressing", "failed", "runtime_failures", "video_packets", "audio_packets", "received_packets", "removed")}}
            await asyncio.sleep(2)
            result["phases"]["native_deleted"] = server_counts()
            stop = time.monotonic()
            publishers["whip"].send_signal(signal.SIGTERM)
            publishers["upstream"].send_signal(signal.SIGTERM)
            remaining = []
            for _ in range(120):
                remaining = sorted(name for name, stream in streams.items() if stream in receivers(base))
                if not remaining:
                    break
                await asyncio.sleep(0.5)
            result["source_stop"] = {"receivers_remaining": remaining, "receivers_gone_after_s": round(time.monotonic() - stop, 1)}
            await asyncio.sleep(5)
            result["source_stop"]["chrome_states"] = {f"{name}/{mode}": await page.evaluate("() => pc.connectionState") for (name, mode), page in pages.items()}
            await browser.close()
        await asyncio.sleep(3)
        result["phases"]["after_cleanup"] = server_counts()
        result["server_alive"] = server.poll() is None
        server.send_signal(signal.SIGTERM)
        result["server_exit"] = server.wait(timeout=30)
    for process in reversed(processes):
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=10)
    for name in flv:
        lines = (args.output / f"flv-{name}.log").read_text().splitlines()
        probe = json.loads(lines[-1]) if lines and lines[-1].startswith("{") else {}
        result["flv"][name] = {key: probe.get(key) for key in ("exit_code", "video_frames", "audio_frames", "audio_pts_gaps", "decode_warnings",
                                                               "av_offset_ms_p50")}
    for key, entry in result["chrome"].items():
        samples = [sample for sample in entry["samples"] if sample.get("frames_decoded")]
        first, last = (samples[0], samples[-1]) if samples else ({}, {})
        span = (last.get("t", 0) - first.get("t", 0)) or None
        entry["summary"] = {"transceivers": last.get("transceivers"), "connection": last.get("connection"),
                            "frames_decoded": (last.get("frames_decoded") or 0) - (first.get("frames_decoded") or 0),
                            "decoded_fps": round(((last.get("frames_decoded") or 0) - (first.get("frames_decoded") or 0)) / span, 1) if span else None,
                            "video_time_advance": round((last.get("video_time") or 0) - (first.get("video_time") or 0), 1),
                            "span_s": span, "freeze_count": last.get("freeze_count"), "frames_dropped": last.get("frames_dropped"),
                            "video_lost": last.get("video_lost"), "audio_packets": last.get("audio_packets"),
                            "size": f"{last.get('width')}x{last.get('height')}"}
    (args.output / "result.json").write_text(json.dumps(result, indent=1))
    print(json.dumps({key: result[key] for key in ("sources_ready", "pull_create", "phases", "fanout", "flv", "source_stop", "server_alive", "server_exit")}))
    for key, entry in result["chrome"].items():
        print("chrome", key, json.dumps({"post": entry["post_status"], "answer": entry["answer"], "delete": entry.get("delete"), **entry["summary"]}))


if __name__ == "__main__":
    main()
