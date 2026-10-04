#!/usr/bin/env python3
"""Exercise the native GB management UI with Chrome and the SIP simulator."""

import argparse
import json
import re
import subprocess
from pathlib import Path

from playwright.sync_api import sync_playwright

from fanout_support import benchmark_head, stop_process, wait_for_listener
from gb_signaling_verify import eventually
from lifecycle_verify import request


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--signaling", type=Path, required=True)
    parser.add_argument("--simulator", type=Path, required=True)
    parser.add_argument("--media", type=Path, default=Path("build/media_server"))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--ffmpeg", default="/home/gyl/bin/ffmpeg")
    parser.add_argument("--browser", default="/usr/bin/google-chrome")
    parser.add_argument("--port-base", type=int, default=43220)
    parser.add_argument("--management-only", action="store_true")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    base = args.port_base
    device, channel = "34020000001320000001", "34020000001320000002"
    device_path = f"/api/devices/{device}"
    processes, commands = [], []
    result = {"head": benchmark_head(), "status": "FAIL", "checks": [], "page_errors": []}
    result["requests"] = []
    result["console"] = []

    def launch(name, command):
        command = list(map(str, command))
        log = args.output / f"{name}.log"
        with log.open("w") as output:
            process = subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT)
        process.log_path = log
        processes.append(process)
        commands.append({"name": name, "command": command})
        return process

    def mark(name):
        result["checks"].append(name)
        print(f"PASS {name}", flush=True)

    def channels():
        return json.loads(request(base+3, "GET", device_path + "/channels")[2])["channels"]

    def decoded(page):
        eventually(lambda: page.evaluate("""async () => {
            const peer = uiPeers.at(-1);
            return peer && peer.connectionState === 'connected' && [...(await peer.getStats()).values()]
                .some(item => item.type === 'inbound-rtp' && item.framesDecoded > 0);
        }"""))

    def stats(page):
        return page.evaluate("""async () => {
            const peer = uiPeers.at(-1), values = [...(await peer.getStats()).values()];
            const video = values.find(item => item.type === 'inbound-rtp' && item.kind === 'video');
            const codec = values.find(item => item.id === video.codecId);
            return {framesDecoded:video.framesDecoded, bytesReceived:video.bytesReceived,
                codec:codec.mimeType, connectionState:peer.connectionState};
        }""")

    try:
        if not args.management_only:
            launch("media", [args.media, "--bind-address", "127.0.0.1", "--webrtc-address", "127.0.0.1",
                "--rtmp-port", base, "--rtsp-port", base+1, "--http-port", base+2, "--threads", "6"])
            wait_for_listener("127.0.0.1", base+2)
        launch("signaling", [args.signaling, "--database", args.output / "devices.db", "--sip-listen", f"127.0.0.1:{base+4}",
            "--sip-advertise", f"127.0.0.1:{base+4}", "--http-listen", f"127.0.0.1:{base+3}",
            "--media-control-url", f"http://127.0.0.1:{base+2}", "--media-http-port", base+2])
        wait_for_listener("127.0.0.1", base+3)
        with sync_playwright() as playwright:
            browser = playwright.chromium.launch(executable_path=args.browser, headless=True)
            result["browser_version"] = browser.version
            try:
                page = browser.new_page(viewport={"width": 1440, "height": 1000}, locale="zh-CN")
                page.add_init_script("""window.uiPeers = []; const NativePeer = window.RTCPeerConnection;
                    window.RTCPeerConnection = class extends NativePeer {
                        constructor(...args) {super(...args); window.uiPeers.push(this);}
                    };""")
                page.on("pageerror", lambda error: result["page_errors"].append(str(error)))
                page.on("console", lambda message: result["console"].append(message.text))
                page.on("response", lambda response: result["requests"].append({"method": response.request.method,
                    "url": response.url, "status": response.status, "location": response.headers.get("location")}))
                page.goto(f"http://127.0.0.1:{base+3}/")
                page.locator("#device-empty").wait_for(state="visible")
                page.locator("#add-device-button").click()
                page.locator("#device-id").fill("123")
                page.locator("#device-name").fill("测试摄像机")
                page.locator("#save-device-button").click()
                page.get_by_text("输入有误，请填写 20 位设备编码和设备名称").wait_for()
                page.locator("#device-id").fill(device)
                page.locator("#save-device-button").click()
                page.locator("#device-dialog").wait_for(state="hidden")
                device_button = page.locator(f"#device-list [data-device-id='{device}']")
                device_button.wait_for()
                assert "测试摄像机" in device_button.inner_text() and "离线" in device_button.inner_text()
                stored = request(base+3, "GET", device_path)
                assert stored[0] == 200 and not json.loads(stored[2])["online"]
                mark("UI adds persistent device and shows name/ID/offline")
                page.locator("#add-device-button").click()
                page.locator("#device-id").fill(device)
                page.locator("#device-name").fill("重复设备")
                page.locator("#save-device-button").click()
                page.get_by_text("设备已存在", exact=True).wait_for()
                page.locator("#cancel-device-dialog").click()
                device_button.click()
                page.get_by_text("设备离线", exact=True).wait_for()
                assert page.locator("#selected-device-name").inner_text() == "测试摄像机"
                mark("duplicate error and offline detail use Chinese text")
                fixture = args.output / "fixture.h264"
                subprocess.run([args.ffmpeg, "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i", "testsrc2=size=320x240:rate=25",
                    "-t", "5", "-c:v", "libx264", "-preset", "ultrafast", "-tune", "zerolatency", "-g", "25", "-bf", "0",
                    "-bsf:v", "h264_metadata=aud=insert", "-an", "-f", "h264", str(fixture)], check=True)
                simulator = launch("simulator", [args.simulator, "--platform-sip", f"127.0.0.1:{base+4}", "--listen", f"127.0.0.1:{base+5}",
                    "--media-file", fixture, "--duration", "5m", "--heartbeat", "1s", "--register-expires", "12s", "--media-workers", "1"])
                page.wait_for_function("document.querySelector('#selected-device-status').textContent.includes('在线')", timeout=15000)
                page.locator(f"[data-channel-id='{channel}'] [data-action='play']").wait_for()
                assert "在线" in device_button.inner_text()
                page.screenshot(path=str(args.output / "devices-online.png"), full_page=True)
                mark("REGISTER and Catalog appear through UI polling")
                if not args.management_only:
                    play = page.locator(f"[data-channel-id='{channel}'] [data-action='play']")
                    play.click()
                    decoded(page)
                    before = stats(page)
                    page.wait_for_timeout(2000)
                    after = stats(page)
                    assert after["framesDecoded"] > before["framesDecoded"] and after["bytesReceived"] > before["bytesReceived"]
                    assert after["codec"] == "video/H264"
                    result["viewer_stats"] = {"before": before, "after": after}
                    live_id = channels()[0]["live"]["live_id"]
                    assert page.locator("#stop-live-button").is_enabled()
                    mark("UI play consumes ticket and Chrome H264 decoder advances")
                    page.locator("#stop-preview-button").click()
                    page.locator(".preview-panel").wait_for(state="hidden")
                    assert page.evaluate("uiPeers.at(-1).connectionState") == "closed"
                    assert channels()[0]["live"]["live_id"] == live_id
                    play.click()
                    decoded(page)
                    assert channels()[0]["live"]["live_id"] == live_id
                    mark("closing viewer leaves shared live; replay rejoins the same generation")
                    page.locator("#stop-live-button").click()
                    eventually(lambda: channels()[0].get("live") is None)
                    page.wait_for_function("document.querySelector('#preview-state').textContent.includes('已结束')")
                    play.click()
                    decoded(page)
                    assert channels()[0]["live"]["live_id"] != live_id
                    mark("UI live stop ends player and replay creates a new generation")
                page.locator("#delete-device-button").click()
                page.get_by_text("删除设备会停止该设备当前所有播放，是否继续？").wait_for()
                page.locator("#confirm-action-button").click()
                page.locator("#device-empty").wait_for()
                assert request(base+3, "GET", device_path)[0] == 404
                assert page.locator("#device-detail").is_hidden()
                if not args.management_only:
                    assert page.evaluate("uiPeers.every(peer => peer.connectionState === 'closed')")
                mark("UI deletion confirms and returns to empty device list")
                assert not result["page_errors"], result["page_errors"]
                stop_process(simulator)
                result["status"] = "PASS"
            finally:
                if result["status"] != "PASS":
                    page.screenshot(path=str(args.output / "failure.png"), full_page=True)
                    result["player_failure"] = page.evaluate("""() => ({text:document.querySelector('#preview-error').textContent,
                        peers:uiPeers.map(peer => ({state:peer.connectionState,local:peer.localDescription?.sdp, remote:peer.remoteDescription?.sdp}))})""")
                browser.close()
    except Exception as error:
        result["error"] = f"{type(error).__name__}: {error}"
        raise
    finally:
        for process in reversed(processes):
            stop_process(process)
        result["process_returncodes"] = {process.log_path.name: process.returncode for process in processes}
        (args.output / "commands.json").write_text(json.dumps(commands, indent=2) + "\n")
        (args.output / "result.json").write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n")


if __name__ == "__main__":
    main()
