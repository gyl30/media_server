#!/usr/bin/env python3
"""Publish Chrome fake camera/microphone to a WHIP URL until SIGTERM."""
import argparse
import asyncio
import json
import signal
from playwright.async_api import async_playwright

PAGE = """async (url) => {
  const stream = await navigator.mediaDevices.getUserMedia({ video: { width: 640, height: 360 }, audio: true });
  const peer = new RTCPeerConnection();
  for (const track of stream.getTracks()) peer.addTransceiver(track, { direction: "sendonly" });
  await peer.setLocalDescription(await peer.createOffer());
  await new Promise(resolve => { if (peer.iceGatheringState === "complete") resolve(); else peer.onicegatheringstatechange = () => peer.iceGatheringState === "complete" && resolve(); });
  const response = await fetch(url, { method: "POST", headers: { "Content-Type": "application/sdp" }, body: peer.localDescription.sdp });
  if (response.status !== 201) return { status: response.status, body: await response.text() };
  window.resource = new URL(response.headers.get("Location"), url).toString();
  await peer.setRemoteDescription({ type: "answer", sdp: await response.text() });
  window.peer = peer;
  return { status: 201, resource: window.resource };
}"""


async def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("url")
    args = parser.parse_args()
    stop = asyncio.Event()
    loop = asyncio.get_running_loop()
    loop.add_signal_handler(signal.SIGTERM, stop.set)
    loop.add_signal_handler(signal.SIGINT, stop.set)
    async with async_playwright() as p:
        browser = await p.chromium.launch(executable_path="/usr/bin/google-chrome", headless=True,
                                          args=["--allow-loopback-in-peer-connection", "--use-fake-ui-for-media-stream", "--use-fake-device-for-media-stream"])
        page = await browser.new_page()
        await page.goto("http://127.0.0.1:39090/favicon.svg")
        print(json.dumps(await page.evaluate(PAGE, args.url)), flush=True)
        while not stop.is_set():
            try:
                await asyncio.wait_for(stop.wait(), 5)
            except asyncio.TimeoutError:
                state = await page.evaluate("""async () => { let sent = 0; (await window.peer.getStats()).forEach(r => { if (r.type === "outbound-rtp" && r.kind === "video") sent = r.framesEncoded; }); return [window.peer.connectionState, sent]; }""")
                print(json.dumps(state), flush=True)
        print(json.dumps(await page.evaluate("async () => { window.peer.close(); const r = await fetch(window.resource, { method: 'DELETE' }); return r.status; }")), flush=True)
        await browser.close()

asyncio.run(main())
