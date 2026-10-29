#!/usr/bin/env python3
"""Resident Chrome page driven over local HTTP: goto/click/fill/press/eval/shot/viewport/log."""
import argparse
import json
import time
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse
from playwright.sync_api import sync_playwright

parser = argparse.ArgumentParser()
parser.add_argument("--port", type=int, default=39500)
parser.add_argument("--shots", required=True)
parser.add_argument("--width", type=int, default=1440)
parser.add_argument("--height", type=int, default=900)
parser.add_argument("--audio", default="")
parser.add_argument("--secure-origin", default="")
args = parser.parse_args()
shots = Path(args.shots)
shots.mkdir(parents=True, exist_ok=True)
logs = []

pw = sync_playwright().start()
browser = pw.chromium.launch(executable_path="/usr/bin/google-chrome", headless=True,
                             args=["--use-fake-ui-for-media-stream", "--use-fake-device-for-media-stream", "--disable-features=LocalNetworkAccessChecks"]
                             + ([f"--use-file-for-fake-audio-capture={args.audio}"] if args.audio else [])
                             + ([f"--unsafely-treat-insecure-origin-as-secure={args.secure_origin}"] if args.secure_origin else []))
context = browser.new_context(viewport={"width": args.width, "height": args.height}, permissions=["microphone"])
context.add_init_script("""(() => {
  const Native = window.RTCPeerConnection;
  window.__pcs = [];
  window.RTCPeerConnection = function (...a) { const pc = new Native(...a); window.__pcs.push(pc); return pc; };
  window.RTCPeerConnection.prototype = Native.prototype;
  window.__rtc = async () => Promise.all(window.__pcs.map(async (pc, i) => {
    const out = { i, state: pc.connectionState, ice: pc.iceConnectionState, tracks: [] };
    if (pc.connectionState === "closed") return out;
    const stats = await pc.getStats();
    stats.forEach(s => {
      if (s.type === "inbound-rtp") {
        const c = stats.get(s.codecId);
        out.tracks.push({ kind: s.kind, codec: c && c.mimeType, clock: c && c.clockRate, fmtp: c && c.sdpFmtpLine, packets: s.packetsReceived, lost: s.packetsLost,
          bytes: s.bytesReceived, framesDecoded: s.framesDecoded, keyFrames: s.keyFramesDecoded, w: s.frameWidth, h: s.frameHeight, fps: s.framesPerSecond,
          samples: s.totalSamplesReceived, concealed: s.concealedSamples, level: s.audioLevel, energy: s.totalAudioEnergy, jitter: s.jitter });
      }
      if (s.type === "candidate-pair" && s.nominated && s.state === "succeeded") {
        const l = stats.get(s.localCandidateId), r = stats.get(s.remoteCandidateId);
        out.pair = (l && l.address + ":" + l.port) + " -> " + (r && r.address + ":" + r.port);
      }
    });
    return out;
  }));
})();""")
page = context.new_page()


def attach(p):
    p.on("console", lambda m: logs.append({"t": time.time(), "type": m.type, "text": m.text}))
    p.on("pageerror", lambda e: logs.append({"t": time.time(), "type": "pageerror", "text": str(e)}))
    p.on("response", lambda r: r.status >= 400 and logs.append({"t": time.time(), "type": "http", "text": f"{r.status} {r.request.method} {r.url}"}))
    p.on("dialog", lambda d: (logs.append({"t": time.time(), "type": "dialog", "text": d.message}), d.dismiss()))


attach(page)


class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        global page
        url = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(url.query).items()}
        body = self.rfile.read(int(self.headers.get("Content-Length", "0"))).decode()
        try:
            op = url.path.strip("/")
            result = None
            if op == "goto":
                page.goto(q["url"])
            elif op == "click":
                page.locator(q["sel"]).first.click(timeout=float(q.get("timeout", 5000)))
            elif op == "fill":
                page.locator(q["sel"]).first.fill(body, timeout=5000)
            elif op == "select":
                page.locator(q["sel"]).first.select_option(body, timeout=5000)
            elif op == "press":
                page.keyboard.press(q["key"])
            elif op == "type":
                page.keyboard.type(body)
            elif op == "eval":
                result = page.evaluate(body)
            elif op == "wait":
                page.wait_for_timeout(float(q.get("ms", 1000)))
            elif op == "waitfor":
                page.wait_for_function(body, timeout=float(q.get("timeout", 30000)), polling=250)
            elif op == "shot":
                path = shots / f"{q['name']}.png"
                if q.get("sel"):
                    page.locator(q["sel"]).first.screenshot(path=str(path))
                else:
                    page.screenshot(path=str(path), full_page=q.get("full") == "1")
                result = str(path)
            elif op == "viewport":
                page.set_viewport_size({"width": int(q["w"]), "height": int(q["h"])})
            elif op == "log":
                result = logs[:]
                logs.clear()
            elif op == "newpage":
                page.close()
                page = context.new_page()
                attach(page)
            elif op == "tab":
                pages = context.pages
                if int(q["i"]) >= len(pages):
                    attach(context.new_page())
                page = context.pages[int(q["i"])]
                page.bring_to_front()
                result = len(context.pages)
            elif op == "quit":
                self.reply(200, {"ok": True})
                raise SystemExit
            else:
                raise ValueError("unknown op " + op)
            self.reply(200, {"ok": True, "result": result})
        except SystemExit:
            raise
        except Exception as error:
            self.reply(500, {"ok": False, "error": str(error)[:2000]})

    def reply(self, code, payload):
        data = json.dumps(payload, ensure_ascii=False).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *_):
        pass


server = HTTPServer(("127.0.0.1", args.port), Handler)
try:
    server.serve_forever()
except SystemExit:
    pass
finally:
    context.close()
    browser.close()
    pw.stop()
