#!/usr/bin/env python3
"""Real Google Chrome WHEP playback through the unmodified signaling/web/whep.js.

The test page and an /api.js ticket stub are served on the media server origin
by Playwright routing; WHEP POST/DELETE go to the real media server.
"""

import argparse
import asyncio
import json
import re
import secrets
import signal
import subprocess
import sys
import time
from pathlib import Path

from playwright.async_api import async_playwright

REPO = Path(__file__).resolve().parents[4]
HOST = "127.0.0.3"
RTMP_PORT, RTSP_PORT, HTTP_PORT, STUB_PORT = 21935, 28554, 28080, 29191
ORIGIN = f"http://{HOST}:{HTTP_PORT}"
FFMPEG = str(Path.home() / "bin/ffmpeg")

PAGE = """<!doctype html><html><body><video id="v" autoplay playsinline muted></video>
<script type="module">
import { WHEPPreview } from "/__test/whep.js";
window.states = [];
window.preview = new WHEPPreview(document.getElementById("v"), (state) => window.states.push({ t: performance.now(), ...state }));
window.stats = async () => {
  const peer = window.preview.current?.peer;
  if (!peer) return null;
  const out = { connectionState: peer.connectionState, codecs: {}, candidates: {} };
  const report = await peer.getStats();
  report.forEach((r) => {
    if (r.type === "inbound-rtp") {
      out[r.kind] = { framesDecoded: r.framesDecoded, framesDropped: r.framesDropped, keyFramesDecoded: r.keyFramesDecoded,
        packetsLost: r.packetsLost, packetsReceived: r.packetsReceived, totalSamplesReceived: r.totalSamplesReceived,
        concealedSamples: r.concealedSamples, frameWidth: r.frameWidth, frameHeight: r.frameHeight, codecId: r.codecId };
    }
    if (r.type === "codec") out.codecs[r.id] = r.mimeType;
    if (r.type === "local-candidate" || r.type === "remote-candidate") out.candidates[r.id] = `${r.address}:${r.port}/${r.protocol}/${r.candidateType}`;
    if (r.type === "candidate-pair" && r.nominated && r.state === "succeeded") out.pair = [r.localCandidateId, r.remoteCandidateId];
  });
  for (const kind of ["video", "audio"]) if (out[kind]) out[kind].codec = out.codecs[out[kind].codecId];
  if (out.pair) out.pair = out.pair.map((id) => out.candidates[id]);
  delete out.codecs; delete out.candidates;
  const video = document.getElementById("v");
  out.videoWidth = video.videoWidth; out.videoHeight = video.videoHeight; out.currentTime = video.currentTime;
  return out;
};
window.ready = true;
</script></body></html>"""

API = """export const api = {
  async play(body, signal) { const response = await fetch("/__test/ticket", { signal }); return response.json(); },
  async playChannel() { throw new Error("unused"); },
};"""


def token():
    return secrets.token_hex(32)


class Harness:
    def __init__(self, args):
        self.args = args
        self.out = Path(args.out)
        self.stream = ""
        self.results = {"chrome": "", "phases": {}, "checks": []}
        self.publisher = None

    def check(self, name, ok, detail=None):
        self.results["checks"].append({"name": name, "ok": bool(ok), "detail": detail})
        print(f"{'PASS' if ok else 'FAIL'} {name} {json.dumps(detail) if detail is not None else ''}", flush=True)

    async def publish(self):
        self.stream = token()
        log = open(self.out / f"publish-{self.stream[:8]}.log", "w")
        self.publisher = await asyncio.create_subprocess_exec(
            FFMPEG, "-nostdin", "-v", "error", "-re", "-stream_loop", "-1", "-i", self.args.fixture, "-c", "copy", "-f", "flv",
            f"rtmp://{HOST}:{RTMP_PORT}/live/{self.stream}", stdout=log, stderr=subprocess.STDOUT)
        await asyncio.sleep(2.5)

    async def route(self, route):
        path = route.request.url[len(ORIGIN):]
        if path == "/api.js":
            await route.fulfill(body=API, content_type="text/javascript")
        elif path == "/__test/whep.js":
            await route.fulfill(body=(REPO / "signaling/web/whep.js").read_text(), content_type="text/javascript")
        elif path == "/favicon.ico":
            await route.fulfill(status=204)
        elif path == "/__test/ticket":
            body = {"stream_id": self.stream, "whep_url": f"{ORIGIN}/play/whep/{self.stream}/{token()}"}
            await route.fulfill(body=json.dumps(body), content_type="application/json")
        else:
            await route.fulfill(body=PAGE, content_type="text/html")

    async def open_page(self, context):
        page = await context.new_page()
        page.on("console", lambda message: self.console.append(f"{message.type}: {message.text}"))
        await page.goto(f"{ORIGIN}/__test/index.html")
        await page.wait_for_function("window.ready === true")
        return page

    async def start(self, page, label):
        return await page.evaluate("(label) => window.preview.start({ source_id: 'acceptance' }, label)", label)

    async def measure(self, page, seconds):
        waited = time.monotonic()
        first = None
        while time.monotonic() - waited < 10:
            first = await page.evaluate("window.stats()")
            if first and (first.get("video") or {}).get("framesDecoded") and (first.get("audio") or {}).get("totalSamplesReceived"):
                break
            await asyncio.sleep(0.1)
        first_media_seconds = round(time.monotonic() - waited, 2)
        await asyncio.sleep(seconds)
        last = await page.evaluate("window.stats()")
        if not first or not last or "video" not in last or "audio" not in last or "video" not in first or "audio" not in first:
            return {"first": first, "last": last, "frames": 0, "samples": 0}
        return {"first": first, "last": last, "first_media_seconds": first_media_seconds,
                "frames": last["video"]["framesDecoded"] - first["video"]["framesDecoded"],
                "samples": last["audio"]["totalSamplesReceived"] - first["audio"]["totalSamplesReceived"]}

    def media_ok(self, measured, seconds):
        last = measured["last"] or {}
        return (measured["frames"] >= 25 * seconds and measured["samples"] >= 40_000 * seconds and last.get("connectionState") == "connected"
                and last.get("videoWidth") == 1280 and last.get("video", {}).get("codec") == "video/H264"
                and last.get("audio", {}).get("codec") == "audio/opus")

    async def wait_state(self, page, wanted, timeout):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            states = await page.evaluate("window.states.map((s) => s.state)")
            if states and states[-1] in wanted:
                return states
            await asyncio.sleep(0.5)
        return await page.evaluate("window.states.map((s) => s.state)")

    def log_count(self, pattern):
        return len(re.findall(pattern, (self.out / "server.log").read_text(errors="replace")))

    async def run(self, playwright):
        browser = await playwright.chromium.launch(executable_path="/usr/bin/google-chrome", headless=True,
                                                   args=["--allow-loopback-in-peer-connection", "--autoplay-policy=no-user-gesture-required"])
        self.results["chrome"] = browser.version
        self.console = []
        context = await browser.new_context()
        await context.route(re.compile(rf"^{re.escape(ORIGIN)}/(__test/.*|api\.js|favicon\.ico)$"), self.route)

        await self.publish()
        page = await self.open_page(context)
        started = await self.start(page, "A")
        states = await self.wait_state(page, {"streaming"}, 15)
        measured = await self.measure(page, 10)
        self.results["phases"]["play"] = {"start": started, "states": states, **measured}
        self.check("play_audio_video_decoded", started is True and self.media_ok(measured, 10),
                   {"frames_10s": measured["frames"], "samples_10s": measured["samples"], "first_media_seconds": measured.get("first_media_seconds"),
                    "video": {key: ((measured["last"] or {}).get("video") or {}).get(key) for key in ("codec", "frameWidth", "frameHeight", "packetsLost", "framesDropped")},
                    "audio": {key: ((measured["last"] or {}).get("audio") or {}).get(key) for key in ("codec", "packetsLost")},
                    "pair": (measured["last"] or {}).get("pair")})

        delete_error = await page.evaluate("window.preview.closeViewer().then((e) => e ? String(e) : null)")
        states = await self.wait_state(page, {"idle"}, 5)
        await asyncio.sleep(1)
        self.check("close_with_delete", delete_error is None and states[-1] == "idle" and self.log_count(r"whep session shutdown") == 1,
                   {"delete_error": delete_error, "last_state": states[-1], "server_shutdowns": self.log_count(r"whep session shutdown")})

        started = await self.start(page, "A2")
        await self.wait_state(page, {"streaming"}, 15)
        measured = await self.measure(page, 6)
        self.results["phases"]["reconnect"] = {"start": started, **measured}
        self.check("reconnect_decoded", started is True and self.media_ok(measured, 6), {"frames_6s": measured["frames"], "samples_6s": measured["samples"]})

        switched = await page.evaluate("Promise.all([window.preview.start({ source_id: 'acceptance' }, 'X'), window.preview.start({ source_id: 'acceptance' }, 'Y')])")
        await self.wait_state(page, {"streaming"}, 15)
        measured = await self.measure(page, 5)
        await asyncio.sleep(1)
        active = self.log_count(r"whep session started") - self.log_count(r"whep session shutdown")
        self.results["phases"]["rapid_switch"] = {"results": switched, "server_active": active, **measured}
        self.check("rapid_switch_latest_wins", switched == [None, True] and self.media_ok(measured, 5) and active == 1,
                   {"results": switched, "server_active_sessions": active, "frames_5s": measured["frames"]})

        others = [await self.open_page(context) for _ in range(3)]
        starts = await asyncio.gather(*(self.start(other, f"P{index}") for index, other in enumerate(others)))
        await asyncio.gather(*(self.wait_state(other, {"streaming"}, 15) for other in others))
        measures = await asyncio.gather(*(self.measure(other, 6) for other in [page, *others]))
        self.results["phases"]["concurrent"] = {"starts": starts, "measures": measures}
        self.check("four_concurrent_viewers", all(item is True for item in starts) and all(self.media_ok(item, 6) for item in measures),
                   {"frames_6s": [item["frames"] for item in measures]})

        shutdowns_before = self.log_count(r"whep session shutdown")
        for other in others[:2]:
            await other.close(run_before_unload=False)
        deadline = time.monotonic() + 45
        while time.monotonic() < deadline and self.log_count(r"whep session shutdown") < shutdowns_before + 2:
            await asyncio.sleep(1)
        closed_after = round(45 - (deadline - time.monotonic()), 1)
        self.check("page_closed_without_delete_cleaned", self.log_count(r"whep session shutdown") >= shutdowns_before + 2,
                   {"seconds": closed_after, "ice_activity_timeouts": self.log_count(r"ice activity timeout")})

        survivor = others[2]
        killed_at = time.monotonic()
        self.publisher.kill()
        await self.publisher.wait()

        async def ended_after(item):
            states = await self.wait_state(item, {"ended", "failed", "idle"}, 45)
            return states, round(time.monotonic() - killed_at, 1)

        timed = await asyncio.gather(*(ended_after(item) for item in (page, survivor)))
        ended = [states for states, _ in timed]
        await asyncio.sleep(1)
        self.results["phases"]["source_end"] = {"states": ended}
        self.check("source_end_reaches_browser", all(states[-1] in {"ended", "failed"} for states in ended)
                   and self.log_count(r"webrtc source stream ended session") >= 2,
                   {"last_states": [states[-1] for states in ended], "seconds_after_source_kill": [seconds for _, seconds in timed],
                    "server_source_ended": self.log_count(r"webrtc source stream ended session")})

        await self.publish()
        started = await self.start(page, "N")
        await self.wait_state(page, {"streaming"}, 15)
        measured = await self.measure(page, 6)
        self.results["phases"]["new_source"] = {"start": started, **measured}
        self.check("new_stream_id_plays", started is True and self.media_ok(measured, 6), {"frames_6s": measured["frames"]})

        started = await self.start(survivor, "S")
        await self.wait_state(survivor, {"streaming"}, 15)
        self.results["console"] = self.console[-40:]
        self.browser = browser


async def wait_port(port, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            _, writer = await asyncio.open_connection(HOST, port)
            writer.close()
            return True
        except OSError:
            await asyncio.sleep(0.2)
    return False


async def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin", required=True)
    parser.add_argument("--stub", required=True)
    parser.add_argument("--fixture", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--threads", default="4")
    args = parser.parse_args()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    stub = subprocess.Popen([sys.executable, args.stub])
    server = subprocess.Popen([args.bin, "--rtmp-port", str(RTMP_PORT), "--rtsp-port", str(RTSP_PORT), "--http-port", str(HTTP_PORT),
                               "--bind-address", HOST, "--webrtc-address", HOST, "--threads", args.threads,
                               "--signaling-url", f"http://127.0.0.1:{STUB_PORT}"],
                              stdout=open(out / "server.log", "w"), stderr=subprocess.STDOUT)
    harness = Harness(args)
    try:
        if not await wait_port(HTTP_PORT, 60):
            raise RuntimeError("server did not listen")
        async with async_playwright() as playwright:
            await harness.run(playwright)
            active = harness.log_count(r"whep session started") - harness.log_count(r"whep session shutdown")
            sigterm_at = time.monotonic()
            server.send_signal(signal.SIGTERM)
            code = await asyncio.wait_for(asyncio.to_thread(server.wait), 120)
            harness.results["shutdown"] = {"active_before_sigterm": active, "exit_code": code, "seconds": round(time.monotonic() - sigterm_at, 2)}
            harness.check("sigterm_with_active_sessions", code in (0, 66) and active == 2
                          and harness.log_count(r"whep session started") == harness.log_count(r"whep session shutdown"),
                          {"active_before_sigterm": active, "exit_code": code, "started": harness.log_count(r"whep session started"),
                           "shutdown": harness.log_count(r"whep session shutdown")})
            await harness.browser.close()
    finally:
        if harness.publisher and harness.publisher.returncode is None:
            harness.publisher.kill()
            await harness.publisher.wait()
        if server.poll() is None:
            server.kill()
            server.wait()
        stub.terminate()
        stub.wait()
        (out / "chrome_whep.json").write_text(json.dumps(harness.results, indent=1))
    failed = [item["name"] for item in harness.results["checks"] if not item["ok"]]
    print(json.dumps({"chrome": harness.results["chrome"], "checks": len(harness.results["checks"]), "failed": failed}))


asyncio.run(main())
