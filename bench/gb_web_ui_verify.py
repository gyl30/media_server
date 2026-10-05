#!/usr/bin/env python3
"""Exercise the native GB management UI with Chrome and the SIP simulator."""

import argparse
import hashlib
import json
import re
import subprocess
import time
from pathlib import Path
from urllib.parse import urljoin, urlsplit

from playwright.sync_api import sync_playwright

from fanout_support import benchmark_head, stop_process, wait_for_listener, wait_for_stream
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
    scope = parser.add_mutually_exclusive_group()
    scope.add_argument("--management-only", action="store_true")
    scope.add_argument("--playback-only", action="store_true")
    scope.add_argument("--rtsp-only", action="store_true")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    base = args.port_base
    device, channel = "34020000001320000001", "34020000001320000002"
    device_path = f"/api/devices/{device}"
    processes, commands = [], []
    result = {"head": benchmark_head(), "status": "FAIL", "checks": [], "page_errors": []}
    result["requests"] = []
    result["console"] = []
    result["source_sha256"] = {str(path): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in [*Path("signaling/web").glob("*"), Path("signaling/media_server_http.go"), Path("signaling/source_control_http.go")]}

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

    def capture(page, name):
        page.evaluate("window.scrollTo({top:0, behavior:'instant'})")
        page.screenshot(path=str(args.output / f"{name}.png"), full_page=True, animations="disabled")
        assert page.evaluate("document.documentElement.scrollWidth <= innerWidth"), name

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
            const audio = values.find(item => item.type === 'inbound-rtp' && item.kind === 'audio');
            const audioCodec = audio && values.find(item => item.id === audio.codecId);
            return {framesDecoded:video.framesDecoded, bytesReceived:video.bytesReceived,
                codec:codec.mimeType, connectionState:peer.connectionState, iceGatheringState:peer.iceGatheringState,
                dtlsConnected:values.some(item => item.dtlsState === 'connected'),
                audio:audio && {codec:audioCodec?.mimeType, samples:audio.totalSamplesReceived, bytesReceived:audio.bytesReceived}};
        }""")

    def resource_gone(resource):
        endpoint = urlsplit(resource)
        eventually(lambda: request(endpoint.port, "GET", endpoint.path)[0] == 404)

    def counters(sim):
        summaries = [line for line in sim.log_path.read_text().splitlines() if "simulator summary" in line]
        return {key: int(value) for key, value in re.findall(r"(\w+)=(\d+)(?=\s|$)", summaries[-1])} if summaries else {}

    def start_simulator(name):
        return launch(name, [args.simulator, "--platform-sip", f"127.0.0.1:{base+4}", "--listen", f"127.0.0.1:{base+5}",
            "--media-file", fixture, "--duration", "5m", "--heartbeat", "1s", "--register-expires", "12s", "--media-workers", "1"])

    started = time.monotonic()
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
            browser = playwright.chromium.launch(executable_path=args.browser, headless=True,
                args=["--autoplay-policy=document-user-activation-required"])
            result["browser_version"] = browser.version
            try:
                def observe_ownership(item):
                    observer = item.context.new_cdp_session(item)
                    observation = observer.send("Runtime.evaluate", {"expression": """(async () => {
                        const {WHEPPreview} = await import('/whep.js');
                        const emit = WHEPPreview.prototype.emit;
                        window.uiOwnership = [];
                        WHEPPreview.prototype.emit = function(...args) {
                            emit.apply(this, args);
                            uiOwnership.push({state:args[0], hasCurrent:this.current !== null,
                                closeDisabled:document.querySelector('#stop-preview-button').disabled,
                                panelHidden:document.querySelector('.preview-panel').hidden});
                        };
                        return document.querySelector('.preview-panel').hidden;
                    })()""", "userGesture": False, "awaitPromise": True, "returnByValue": True})
                    assert observation["result"].get("value") is True
                    observer.detach()

                def open_page():
                    item = browser.new_page(viewport={"width": 1440, "height": 1000}, locale="zh-CN")
                    item.resources = []
                    item.add_init_script("""window.uiPeers = []; window.uiFocus = [];
                        document.addEventListener('focusin', event => uiFocus.push({time:performance.now(),
                            id:event.target.id, data:{...event.target.dataset}}));
                        const NativePeer = window.RTCPeerConnection;
                        window.RTCPeerConnection = class extends NativePeer {
                            constructor(...args) {super(...args); window.uiPeers.push(this);}
                        };""")
                    item.on("pageerror", lambda error: result["page_errors"].append(str(error)))
                    item.on("console", lambda message: result["console"].append(message.text))
                    def response_record(response):
                        location = response.headers.get("location")
                        result["requests"].append({"method": response.request.method, "url": response.url,
                            "status": response.status, "location": location})
                        if response.request.method == "POST" and response.status == 201 and location:
                            item.resources.append(urljoin(response.url, location))
                    item.on("response", response_record)
                    item.goto(f"http://127.0.0.1:{base+3}/")
                    observe_ownership(item)
                    return item

                def select_device(item):
                    item.locator(f"#device-list [data-device-id='{device}']").click()
                    item.locator(f"[data-channel-id='{channel}'] [data-action='play']").wait_for()

                def play_channel(item):
                    item.locator(f"[data-channel-id='{channel}'] [data-action='play']").click()
                    decoded(item)
                    focused(item, f"[data-channel-id='{channel}'][data-action='play']")

                def focused(item, selector):
                    try:
                        item.wait_for_function("selector => document.activeElement !== document.body && document.activeElement.matches(selector)", arg=selector, timeout=3000)
                    except Exception:
                        result["focus_failure"] = item.evaluate("""() => ({active:{id:document.activeElement.id,
                            tag:document.activeElement.tagName, data:{...document.activeElement.dataset}},
                            history:uiFocus.slice(-20),
                            controls:[...document.querySelectorAll('button')].filter(button => button.checkVisibility())
                                .map(button => ({id:button.id, data:{...button.dataset}, disabled:button.disabled}))})""")
                        raise
                    result.setdefault("focus_targets", []).append(selector)

                def recover_poll(item, path, error_selector, error_text):
                    def poll_failure(route):
                        if route.request.method == "GET": route.fulfill(status=503, json={"error": "network_error"})
                        else: route.continue_()
                    item.route(path, poll_failure)
                    item.locator("#global-status.danger").wait_for()
                    last_update = item.locator("#last-updated").get_attribute("datetime")
                    item.unroute(path, poll_failure)
                    item.wait_for_function("previous => document.querySelector('#last-updated').dateTime !== previous", arg=last_update)
                    item.locator("#global-status").wait_for(state="hidden", timeout=8000)
                    assert item.locator(error_selector).inner_text() == error_text
                    assert item.locator(error_selector).is_visible()
                    mark(f"poll recovery expires transient danger while {error_selector} remains inline")

                def submit_pending(item, kind, path):
                    held = []
                    def hold(route):
                        if route.request.method == "POST": held.append(route)
                        else: route.continue_()
                    item.route(path, hold)
                    item.locator(f"#save-{kind}-button").click()
                    item.wait_for_function(f"document.querySelector('#save-{kind}-button').disabled")
                    assert len(held) == 1
                    for control in (f"save-{kind}-button", f"close-{kind}-dialog", f"cancel-{kind}-dialog"):
                        assert item.locator(f"#{control}").is_disabled(), control
                    item.keyboard.press("Escape")
                    assert item.locator(f"#{kind}-dialog").is_visible()
                    item.locator(f"#save-{kind}-button").evaluate("button => button.click()")
                    assert len(held) == 1
                    held[0].fulfill(response=held[0].fetch())
                    item.locator(f"#{kind}-dialog").wait_for(state="hidden")
                    item.wait_for_function(f"!document.querySelector('#save-{kind}-button').disabled && !document.querySelector('#refresh-button').disabled")
                    item.unroute(path, hold)
                    item.wait_for_function(f"document.activeElement.id === 'add-{kind}-button'")
                    mark(f"{kind} submit blocks close/cancel/Esc/reentry and restores trigger focus")

                page = open_page()
                page.locator("#device-empty").wait_for(state="visible")
                capture(page, "empty")
                page.locator("#tab-sources").click()
                page.locator("#add-source-button").click()
                page.locator("#source-stream-name").fill("rtsp/dialog-check")
                page.locator("#source-url").fill(f"rtsp://127.0.0.1:{base+1}/dialog-check")
                page.locator("#source-password").fill("invalid-without-username")
                page.locator("#save-source-button").click()
                page.locator("#source-form-error").wait_for()
                recover_poll(page, "**/api/sources", "#source-form-error", "设置密码前请输入用户名")
                page.locator("#source-password").fill("")
                submit_pending(page, "source", "**/api/sources")
                page.locator("#source-rows [data-action='delete']").click()
                page.keyboard.press("Escape")
                page.locator("#confirm-dialog").wait_for(state="hidden")
                focused(page, "#source-rows [data-action='delete']")
                page.locator("#source-rows [data-action='delete']").click()
                page.locator("#confirm-action-button").click()
                page.locator("#source-empty").wait_for()
                focused(page, "#add-source-button")
                assert request(base+3, "POST", "/api/sources", {"stream_name": "rtsp/poll-removal",
                    "url": f"rtsp://127.0.0.1:{base+1}/poll-removal"})[0] == 201
                page.locator("#source-rows [data-action='edit']").wait_for()
                page.locator("#source-rows [data-action='edit']").focus()
                source_id = json.loads(request(base+3, "GET", "/api/sources")[2])["sources"][0]["source_id"]
                assert request(base+3, "DELETE", f"/api/sources/{source_id}")[0] == 204
                page.locator("#source-empty").wait_for()
                focused(page, "#add-source-button")
                mark("poll removal of a focused resource restores its list entry point")
                page.evaluate("location.hash = 'devices'")
                focused(page, "#tab-devices")
                page.locator("#add-device-button").click()
                page.locator("#device-id").fill("123")
                page.locator("#device-name").fill("测试摄像机")
                page.locator("#save-device-button").click()
                page.get_by_text("输入有误，请填写 20 位设备编码和设备名称").wait_for()
                page.locator("#device-id").fill(device)
                submit_pending(page, "device", "**/api/devices")
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
                recover_poll(page, "**/api/devices", "#device-form-error", "设备已存在")
                page.locator("#cancel-device-dialog").click()
                focused(page, "#add-device-button")
                device_button.click()
                page.get_by_text("设备离线", exact=True).wait_for()
                assert page.locator("#selected-device-name").inner_text() == "测试摄像机"
                capture(page, "offline")
                mark("duplicate error and offline detail use Chinese text")
                page.locator("#back-devices-button").click()
                focused(page, f"#device-list [data-device-id='{device}']")
                device_button.click()
                assert not any("/channels" in item["url"] for item in result["requests"])
                fixture = args.output / "fixture.h264"
                subprocess.run([args.ffmpeg, "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i", "testsrc2=size=320x240:rate=25",
                    "-t", "5", "-c:v", "libx264", "-preset", "ultrafast", "-tune", "zerolatency", "-g", "25", "-bf", "0",
                    "-bsf:v", "h264_metadata=aud=insert", "-an", "-f", "h264", str(fixture)], check=True)
                simulator = start_simulator("simulator")
                page.wait_for_function("document.querySelector('#selected-device-status').textContent.includes('在线')", timeout=15000)
                page.locator(f"[data-channel-id='{channel}'] [data-action='play']").wait_for()
                assert "在线" in device_button.inner_text()
                capture(page, "devices-online")
                mark("REGISTER and Catalog appear through UI polling")
                if not args.management_only and not args.rtsp_only:
                    ice_page = open_page()
                    select_device(ice_page)
                    await_start = len(result["requests"])
                    ice_page.evaluate("""async () => {
                        const {WHEPPreview} = await import('/whep.js');
                        const local = RTCPeerConnection.prototype.setLocalDescription;
                        const start = WHEPPreview.prototype.start;
                        window.uiStartSettled = false;
                        WHEPPreview.prototype.start = async function(...args) {
                            try {return await start.apply(this, args);}
                            finally {window.uiStartSettled = true;}
                        };
                        RTCPeerConnection.prototype.setLocalDescription = async function(...args) {
                            await local.apply(this, args);
                            Object.defineProperty(this, 'iceGatheringState', {configurable:true, get:() => 'gathering'});
                            await new Promise(resolve => {window.uiReleaseLocal = resolve;});
                        };
                    }""")
                    ice_page.locator(f"[data-channel-id='{channel}'] [data-action='play']").click()
                    ice_page.wait_for_function("typeof uiReleaseLocal === 'function'")
                    ice_page.locator("#stop-preview-button").click()
                    ice_page.locator(".preview-panel").wait_for(state="hidden")
                    ice_page.evaluate("uiReleaseLocal()")
                    ice_page.wait_for_function("uiStartSettled", timeout=3000)
                    assert ice_page.evaluate("uiPeers.at(-1).connectionState") == "closed"
                    assert not ice_page.resources
                    assert not any(item["method"] == "POST" for item in result["requests"][await_start:])
                    ice_page.close()
                    mark("cancel during native local-description await settles start without ICE events or POST")
                    play = page.locator(f"[data-channel-id='{channel}'] [data-action='play']")
                    channel_route = f"**{device_path}/channels"
                    def offline_channel(route):
                        response = route.fetch()
                        payload = response.json()
                        payload["channels"][0]["status"] = "OFF"
                        route.fulfill(response=response, json=payload)
                    page.route(channel_route, offline_channel)
                    page.wait_for_function("document.querySelector('#channel-rows [data-action=play]').disabled")
                    assert "离线" in page.locator(f"#channel-rows tr[data-channel-id='{channel}']").inner_text()
                    page.unroute(channel_route, offline_channel)
                    page.wait_for_function("!document.querySelector('#channel-rows [data-action=play]').disabled")
                    mark("OFF channel cannot be played and offline device never polls channels")
                    play.click()
                    assert play.is_disabled()
                    play.evaluate("button => button.click()")
                    decoded(page)
                    focused(page, f"[data-channel-id='{channel}'][data-action='play']")
                    assert page.evaluate("uiPeers.length") == 1
                    before = stats(page)
                    page.wait_for_timeout(2000)
                    after = stats(page)
                    assert after["framesDecoded"] > before["framesDecoded"] and after["bytesReceived"] > before["bytesReceived"]
                    assert after["codec"] == "video/H264"
                    result["viewer_stats"] = {"before": before, "after": after}
                    assert after["iceGatheringState"] == "complete" and after["dtlsConnected"]
                    live_id = channels()[0]["live"]["live_id"]
                    assert live_id not in page.locator("body").inner_text()
                    for item in result["requests"]:
                        if item["method"] == "POST" and "/play/whep/" in item["url"]:
                            assert urlsplit(item["url"]).path.split("/")[-1] not in page.locator("body").inner_text()
                    assert page.locator("#stop-live-button").is_enabled()
                    mark("UI play consumes ticket and Chrome H264 decoder advances")
                    viewers = [page]
                    if not args.playback_only:
                        for _ in range(2):
                            other = open_page()
                            select_device(other)
                            play_channel(other)
                            viewers.append(other)
                        before_all = [stats(item) for item in viewers]
                        page.wait_for_timeout(2000)
                        after_all = [stats(item) for item in viewers]
                        assert all(b["framesDecoded"] > a["framesDecoded"] for a, b in zip(before_all, after_all))
                        count = counters(simulator)
                        assert count["invite"] == count["ack"] == count["live_active"] == 1, count
                        result["multi_viewer"] = {"before": before_all, "after": after_all, "counters": count}
                        mark("three independent UI viewers decode from one INVITE and one active upstream")
                    failed_resource = page.resources[-1]
                    before_failure = counters(simulator)
                    invite_before_failure = before_failure["invite"]
                    failed_ticket = next(item["url"] for item in reversed(result["requests"])
                        if item["method"] == "POST" and item["location"] == failed_resource)
                    requests_before_failure = len(result["requests"])
                    page.evaluate("""() => {
                        const peer = uiPeers.at(-1);
                        Object.defineProperty(peer, 'connectionState', {configurable:true, get:() => 'failed'});
                        peer.dispatchEvent(new Event('connectionstatechange'));
                        delete peer.connectionState;
                    }""")
                    result["transport_failure"] = page.evaluate("""() => ({
                        state:document.querySelector('#preview-state').textContent,
                        error:document.querySelector('#preview-error').textContent,
                        ownership:uiOwnership.at(-1), peer:uiPeers.at(-1).connectionState,
                        videoCleared:document.querySelector('#preview-video').srcObject === null,
                        stopLiveHidden:document.querySelector('#stop-live-button').hidden})""")
                    failure = result["transport_failure"]
                    assert "播放失败" in failure["state"] and failure["error"] == "连接中断，请重新播放", failure
                    assert not failure["ownership"]["hasCurrent"] and not failure["ownership"]["closeDisabled"], failure
                    assert failure["peer"] == "closed" and failure["videoCleared"] and failure["stopLiveHidden"], failure
                    capture(page, "transport-failed")
                    resource_gone(failed_resource)
                    page.locator("#global-status").wait_for(state="hidden", timeout=7000)
                    assert page.locator("#preview-error").is_visible()
                    dismiss_start = len(result["requests"])
                    page.locator("#stop-preview-button").click()
                    page.locator(".preview-panel").wait_for(state="hidden")
                    focused(page, "#add-device-button")
                    assert not any(item["method"] == "DELETE" for item in result["requests"][dismiss_start:])
                    mark("failed panel keeps inline error then dismisses without network cleanup")
                    page.wait_for_timeout(1500)
                    assert channels()[0]["live"]["live_id"] == live_id
                    eventually(lambda: counters(simulator)["rtp_packets"] > before_failure["rtp_packets"])
                    count = counters(simulator)
                    assert count["invite"] == invite_before_failure and count["live_active"] == 1
                    assert not any(item["method"] == "DELETE" and "/api/lives/" in item["url"]
                        for item in result["requests"][requests_before_failure:])
                    page.locator(f"[data-channel-id='{channel}']").get_by_text("正在取流", exact=True).wait_for()
                    play_channel(page)
                    page.wait_for_timeout(1500)
                    assert channels()[0]["live"]["live_id"] == live_id and counters(simulator)["invite"] == invite_before_failure
                    assert page.resources[-1] != failed_resource
                    replay_ticket = next(item["url"] for item in reversed(result["requests"])
                        if item["method"] == "POST" and item["location"] == page.resources[-1])
                    assert replay_ticket != failed_ticket
                    failure.update({"live_preserved": True, "replay_same_live": True,
                        "invite_before": invite_before_failure, "invite_after_replay": counters(simulator)["invite"],
                        "rtp_packets_before": before_failure["rtp_packets"], "rtp_packets_after_failure": count["rtp_packets"],
                        "fresh_ticket": True,
                        "injection": "connectionstatechange on a real connected/decoding peer; no PeerConnection replacement"})
                    mark("transport failure clears viewer with failure text; replay keeps live and INVITE unchanged")
                    held_close = []
                    def delayed_close(route):
                        if route.request.method == "DELETE": held_close.append(route)
                        else: route.continue_()
                    close_route = "**/play/whep/session/*"
                    page.route(close_route, delayed_close)
                    page.locator("#stop-preview-button").click()
                    page.wait_for_function("document.querySelector('#preview-state').textContent.includes('正在关闭')")
                    assert page.locator("#stop-preview-button").is_disabled()
                    page.locator("#stop-preview-button").evaluate("button => button.click()")
                    assert len(held_close) == 1
                    capture(page, "viewer-stopping")
                    assert len(held_close) == 1
                    held_close[0].fulfill(response=held_close[0].fetch())
                    page.unroute(close_route, delayed_close)
                    page.locator(".preview-panel").wait_for(state="hidden")
                    resource_gone(page.resources[-1])
                    focused(page, f"[data-channel-id='{channel}'][data-action='play']")
                    assert page.evaluate("uiPeers.at(-1).connectionState") == "closed"
                    assert page.evaluate("document.querySelector('#preview-video').srcObject === null")
                    assert page.locator(".preview-panel").is_hidden()
                    assert channels()[0]["live"]["live_id"] == live_id
                    play.click()
                    decoded(page)
                    assert channels()[0]["live"]["live_id"] == live_id
                    mark("closing viewer leaves shared live; replay rejoins the same generation")
                    old_resource = page.resources[-1]
                    def delete_viewer_failure(route):
                        if route.request.method == "DELETE": route.fulfill(status=502)
                        else: route.continue_()
                    page.route(close_route, delete_viewer_failure)
                    page.locator("#stop-preview-button").click()
                    page.locator(".preview-panel").wait_for(state="hidden")
                    assert page.evaluate("uiPeers.at(-1).connectionState") == "closed"
                    assert page.evaluate("document.querySelector('#preview-video').srcObject === null")
                    page.get_by_text("播放器已关闭，服务器资源尚待清理", exact=True).wait_for(timeout=3000)
                    focused(page, f"[data-channel-id='{channel}'][data-action='play']")
                    page.unroute(close_route, delete_viewer_failure)
                    endpoint = urlsplit(old_resource)
                    assert request(endpoint.port, "DELETE", endpoint.path)[0] in (204, 404)
                    play_channel(page)
                    mark("explicit viewer DELETE 502 closes locally, warns about cleanup and restores play focus")
                    spare_device = "34020000001320000003"
                    assert request(base+3, "POST", "/api/devices", {"device_id": spare_device, "name": "切换检查"})[0] == 201
                    page.locator(f"#device-list [data-device-id='{spare_device}']").wait_for()
                    for action in ("replay", "tab", "device"):
                        old_resource = page.resources[-1]
                        page.route(close_route, delete_viewer_failure)
                        if action == "replay":
                            play_channel(page)
                        elif action == "tab":
                            page.locator("#tab-sources").click()
                            page.locator(".preview-panel").wait_for(state="hidden")
                            focused(page, "#tab-sources")
                            page.locator("#tab-devices").click()
                            play_channel(page)
                        else:
                            page.locator(f"#device-list [data-device-id='{spare_device}']").click()
                            page.locator(".preview-panel").wait_for(state="hidden")
                            focused(page, f"#device-list [data-device-id='{spare_device}']")
                            select_device(page)
                            play_channel(page)
                        assert page.locator("#global-status").inner_text() != "播放器已关闭，服务器资源尚待清理"
                        page.unroute(close_route, delete_viewer_failure)
                        endpoint = urlsplit(old_resource)
                        assert request(endpoint.port, "DELETE", endpoint.path)[0] in (204, 404)
                    assert request(base+3, "DELETE", f"/api/devices/{spare_device}")[0] == 204
                    mark("automatic replay/tab/device cleanup ignores remote DELETE errors without stale UI or warnings")
                    if not args.playback_only:
                        invite_count = counters(simulator)["invite"]
                        result["button_ownership"] = page.evaluate("uiOwnership")
                        page.reload()
                        observe_ownership(page)
                        resource_gone(page.resources[-1])
                        select_device(page)
                        page.locator(f"[data-channel-id='{channel}']").get_by_text("正在取流", exact=True).wait_for()
                        play_channel(page)
                        assert channels()[0]["live"]["live_id"] == live_id
                        assert counters(simulator)["invite"] == invite_count
                        mark("reload shows existing live and UI replay does not issue another INVITE")
                    stop_route = "**/api/lives/*"
                    def stop_failure(route):
                        if route.request.method == "DELETE": route.fulfill(status=502, json={"error": "live_stop_failed"})
                        else: route.continue_()
                    page.route(stop_route, stop_failure)
                    page.locator("#stop-live-button").click()
                    page.locator("#global-status.danger").wait_for()
                    page.wait_for_function("!document.querySelector('#stop-live-button').disabled")
                    assert channels()[0]["live"]["live_id"] == live_id
                    assert page.evaluate("uiPeers.at(-1).connectionState") == "connected"
                    capture(page, "stop-error")
                    page.unroute(stop_route, stop_failure)
                    mark("stop request failure leaves the live and viewer usable")
                    page.locator("#channel-rows [data-action='stop']").click()
                    eventually(lambda: channels()[0].get("live") is None)
                    focused(page, f"[data-channel-id='{channel}'][data-action='play']")
                    page.wait_for_function("document.querySelector('#preview-state').textContent.includes('已结束')")
                    assert page.locator("#preview-error").inner_text() == "设备已离线或媒体已结束"
                    assert page.locator("#stop-preview-button").is_enabled()
                    page.locator("#global-status").wait_for(state="hidden", timeout=7000)
                    assert page.locator("#preview-error").is_visible()
                    capture(page, "media-ended")
                    resource_gone(page.resources[-1])
                    dismiss_start = len(result["requests"])
                    page.locator("#stop-preview-button").click()
                    page.locator(".preview-panel").wait_for(state="hidden")
                    assert not any(item["method"] == "DELETE" for item in result["requests"][dismiss_start:])
                    focused(page, "#add-device-button")
                    mark("ended panel dismisses without a second viewer DELETE")
                    for other in viewers[1:]:
                        other.wait_for_function("document.querySelector('#preview-state').textContent.includes('已结束')", timeout=15000)
                        assert other.locator("#preview-error").inner_text() == "设备已离线或媒体已结束"
                        assert other.locator("#stop-preview-button").is_enabled()
                        assert other.evaluate("uiPeers.every(peer => peer.connectionState === 'closed')")
                        resource_gone(other.resources[-1])
                    if not args.playback_only:
                        mark("stopping live ends all three UI viewers")
                    play.click()
                    decoded(page)
                    assert channels()[0]["live"]["live_id"] != live_id
                    mark("UI live stop ends player and replay creates a new generation")
                    if not args.playback_only:
                        stopped_generation = channels()[0]["live"]["live_id"]
                        stop_process(simulator)
                        assert simulator.returncode == 0
                        page.wait_for_function("document.querySelector('#selected-device-status').textContent.includes('离线')", timeout=15000)
                        page.wait_for_function("document.querySelector('#preview-state').textContent.includes('已结束')")
                        assert page.locator("#preview-error").inner_text() == "设备已离线或媒体已结束"
                        assert page.locator("#stop-preview-button").is_enabled()
                        assert page.locator("#channel-rows tr").count() == 0 and channels() == []
                        focused(page, "#add-device-button")
                        mark("Expires:0 clears channels and ends the UI player while device stays visible")
                        simulator = start_simulator("simulator-reregister")
                        page.wait_for_function("document.querySelector('#selected-device-status').textContent.includes('在线')", timeout=15000)
                        play_channel(page)
                        assert channels()[0]["live"]["live_id"] != stopped_generation
                        mark("REGISTER after offline restores Catalog and UI playback")
                        for width, height in ((1440, 900), (1920, 1080), (1000, 850)):
                            page.set_viewport_size({"width": width, "height": height})
                            capture(page, f"player-{width}")
                        page.set_viewport_size({"width": 1440, "height": 1000})
                        capture(page, "player")
                        page.set_viewport_size({"width": 600, "height": 950})
                        assert page.evaluate("document.documentElement.scrollWidth <= innerWidth")
                        capture(page, "narrow")
                        page.set_viewport_size({"width": 1440, "height": 1000})

                        # Hold a real ticket response until the server's 30 second expiry.
                        held = []
                        play_route = f"**{device_path}/channels/{channel}/play"
                        def expired_ticket(route):
                            if not held:
                                held.append((route, route.fetch()))
                            else:
                                route.continue_()
                        page.route(play_route, expired_ticket)
                        expired_request_start = len(result["requests"])
                        play.click()
                        page.wait_for_timeout(31000)
                        assert len(held) == 1 and play.is_disabled()
                        held[0][0].fulfill(response=held[0][1])
                        decoded(page)
                        page.unroute(play_route, expired_ticket)
                        expired_offers = [item for item in result["requests"][expired_request_start:]
                            if item["method"] == "POST" and "/play/whep/" in item["url"]]
                        assert [item["status"] for item in expired_offers] == [404, 201]
                        assert len({item["url"] for item in expired_offers}) == 2
                        result["ticket_expiry"] = {"wait_seconds": 31, "offer_statuses": [404, 201], "fresh_ticket_retries": 1}
                        mark("real 30 second ticket expiry triggers one fresh ticket and UI playback recovers")

                        failure_route = "**/play/whep/*"
                        def invalid_ticket(route):
                            route.fulfill(status=404)
                        page.route(failure_route, invalid_ticket)
                        request_start = len(result["requests"])
                        play.click()
                        page.wait_for_function("document.querySelector('#preview-state').textContent.includes('播放失败')")
                        page.wait_for_timeout(2500)
                        failed_offers = [item for item in result["requests"][request_start:]
                            if item["method"] == "POST" and "/play/whep/" in item["url"]]
                        assert len(failed_offers) == 2 and len({item["url"] for item in failed_offers}) == 2
                        page.unroute(failure_route, invalid_ticket)
                        mark("two invalid tickets stop with Chinese failure and no unbounded retry")
                        play_channel(page)

                        def delete_failure(route):
                            if route.request.method == "DELETE": held_delete.append(route)
                            else: route.continue_()
                        held_delete = []
                        page.route(f"**{device_path}", delete_failure)
                        page.locator("#delete-device-button").click()
                        page.locator("#confirm-action-button").click()
                        page.wait_for_function("document.querySelector('#delete-device-button').disabled")
                        page.wait_for_timeout(100)
                        assert len(held_delete) == 1
                        page.locator("#add-device-button").focus()
                        held_delete[0].fulfill(status=502, content_type="application/json", body='{"error":"device_delete_failed"}')
                        page.get_by_text("设备暂时无法删除，媒体资源清理未完成，请稍后重试。", exact=True).wait_for()
                        assert not page.locator("#device-detail").is_hidden() and request(base+3, "GET", device_path)[0] == 200
                        assert page.evaluate("uiPeers.at(-1).connectionState") == "connected"
                        page.wait_for_function("!document.querySelector('#delete-device-button').disabled")
                        focused(page, "#add-device-button")
                        page.unroute(f"**{device_path}", delete_failure)
                        mark("delete pending disables controls; cleanup failure keeps device/detail/player")

                        held_offer = []
                        def delayed_offer(route):
                            if route.request.method == "POST": held_offer.append((route, route.fetch()))
                            else: route.continue_()
                        page.route(failure_route, delayed_offer)
                        play.click()
                        page.wait_for_timeout(500)
                        assert len(held_offer) == 1 and held_offer[0][1].status == 201
                        capture(page, "connecting")
                        page.locator("#back-devices-button").click()
                        focused(page, f"#device-list [data-device-id='{device}']")
                        assert page.evaluate("uiPeers.at(-1).connectionState") == "closed"
                        late_resource = held_offer[0][1].headers["location"]
                        held_offer[0][0].fulfill(response=held_offer[0][1])
                        resource_gone(late_resource)
                        assert page.locator(".preview-panel").is_hidden() and page.locator("#device-detail").is_hidden()
                        page.unroute(failure_route, delayed_offer)
                        select_device(page)
                        play_channel(page)
                        mark("back during pending offer closes peer and deletes the late resource without reviving player")
                    ownership = result.get("button_ownership", []) + page.evaluate("uiOwnership")
                    assert {"idle", "preparing", "streaming", "failed", "ended", "stopping"} <= {item["state"] for item in ownership}
                    assert all(item["panelHidden"] for item in ownership if item["state"] == "idle"), ownership
                    assert all(item["closeDisabled"] for item in ownership if item["state"] == "stopping"), ownership
                    assert all(not item["closeDisabled"] for item in ownership if item["state"] not in ("idle", "stopping")), ownership
                    result["button_ownership"] = ownership
                    mark("panel dismiss stays available after viewer release; stopping blocks repeated close")
                page.locator("#delete-device-button").click()
                page.get_by_text("删除设备会停止该设备当前所有播放，是否继续？").wait_for()
                page.locator("#confirm-action-button").click()
                page.locator("#device-empty").wait_for()
                assert request(base+3, "GET", device_path)[0] == 404
                focused(page, "#add-device-button")
                assert page.locator("#device-detail").is_hidden()
                if not args.management_only:
                    assert page.evaluate("uiPeers.every(peer => peer.connectionState === 'closed')")
                    assert page.locator(".preview-panel").is_hidden()
                    if page.resources: resource_gone(page.resources[-1])
                mark("UI deletion confirms and returns to empty device list")
                if not args.management_only and not args.playback_only:
                    stop_process(simulator)
                    rejected = start_simulator("simulator-rejected")
                    assert rejected.wait(timeout=10) != 0 and "REGISTER challenge status 403" in rejected.log_path.read_text()
                    mark("deleted device REGISTER is rejected with 403")

                    audio_fixture = args.output / "audio-video.mkv"
                    subprocess.run([args.ffmpeg, "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i", "testsrc2=size=320x240:rate=25",
                        "-f", "lavfi", "-i", "sine=frequency=1000:sample_rate=48000", "-t", "8", "-c:v", "libx264", "-preset", "ultrafast",
                        "-tune", "zerolatency", "-g", "25", "-bf", "0", "-c:a", "aac", "-ac", "2", str(audio_fixture)], check=True)
                    launch("audio-publisher", [args.ffmpeg, "-hide_banner", "-loglevel", "error", "-stream_loop", "-1", "-re", "-i", audio_fixture,
                        "-c", "copy", "-f", "flv", f"rtmp://127.0.0.1:{base}/live/ui-audio"])
                    wait_for_stream("127.0.0.1", base+2, "live/ui-audio")
                    page.locator("#tab-sources").click()
                    page.locator("#add-source-button").click()
                    page.locator("#source-stream-name").fill("rtsp/ui-audio")
                    page.locator("#source-url").fill(f"rtsp://127.0.0.1:{base+1}/live/ui-audio")
                    page.locator("#save-source-button").click()
                    source_row = page.locator("#source-rows tr").filter(has_text="rtsp/ui-audio")
                    source_row.locator("[data-action='edit']").click()
                    page.locator("#source-stream-name").fill("rtsp/ui-audio-edited")
                    page.locator("#save-source-button").click()
                    source_row.locator("[data-action='start']").click()
                    source_row.locator("[data-action='preview']").wait_for()
                    focused(page, "#source-rows [data-action='stop']")
                    source_row.locator("[data-action='preview']").click()
                    decoded(page)
                    eventually(lambda: ((stats(page).get("audio") or {}).get("samples", 0) or 0) > 0)
                    before = stats(page)
                    page.wait_for_timeout(2000)
                    after = stats(page)
                    assert after["framesDecoded"] > before["framesDecoded"] and after["audio"]["samples"] > before["audio"]["samples"]
                    assert after["codec"] == "video/H264" and after["audio"]["codec"] == "audio/opus"
                    assert not page.evaluate("document.querySelector('#preview-video').muted")
                    result["audio_video"] = {"before": before, "after": after}
                    capture(page, "rtsp-player")
                    mark("retained RTSP UI add/edit/start/preview uses the same H264/Opus player")
                    autoplay = open_page()
                    autoplay.goto(f"http://127.0.0.1:{base+3}/#sources")
                    cdp = autoplay.context.new_cdp_session(autoplay)
                    def without_gesture(expression):
                        return cdp.send("Runtime.evaluate", {"expression": expression, "userGesture": False,
                            "returnByValue": True, "awaitPromise": True})["result"].get("value")
                    eventually(lambda: without_gesture("Boolean(document.querySelector('#source-rows [data-action=preview]'))"))
                    assert not without_gesture("navigator.userActivation.hasBeenActive")
                    without_gesture("document.querySelector('#source-rows [data-action=preview]').click()")
                    eventually(lambda: without_gesture("!document.querySelector('#resume-playback-button').hidden"))
                    autoplay.locator("#resume-playback-button").click()
                    decoded(autoplay)
                    autoplay.wait_for_function("!document.querySelector('#preview-video').paused")
                    focused(autoplay, "#stop-preview-button")
                    assert not autoplay.evaluate("document.querySelector('#preview-video').muted")
                    autoplay.locator("#stop-preview-button").click()
                    resource_gone(autoplay.resources[-1])
                    mark("Chrome autoplay denial shows an explicit unmuted playback button")
                    source_row.locator("[data-action='stop']").click()
                    source_row.locator("[data-action='start']").wait_for()
                    focused(page, "#source-rows [data-action='start']")
                    assert page.evaluate("uiPeers.every(peer => peer.connectionState === 'closed')")
                    resource_gone(page.resources[-1])
                    source_row.locator("[data-action='start']").click()
                    source_row.locator("[data-action='preview']").click()
                    decoded(page)
                    source_resource = page.resources[-1]
                    source_row.locator("[data-action='delete']").click()
                    page.locator("#confirm-action-button").click()
                    page.locator("#source-empty").wait_for()
                    focused(page, "#add-source-button")
                    page.wait_for_function("uiPeers.every(peer => peer.connectionState === 'closed')", timeout=5000)
                    resource_gone(source_resource)
                    mark("RTSP UI stop/restart/delete preserve operations and release its viewer")
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
        unexpected = {name: code for name, code in result["process_returncodes"].items()
            if code not in ({1} if name == "simulator-rejected.log" else {0, 255} if name == "audio-publisher.log" else {0})}
        if unexpected:
            result["status"] = "FAIL"
            result["cleanup_error"] = f"unexpected process exits: {unexpected}"
        result["duration_seconds"] = time.monotonic() - started
        (args.output / "commands.json").write_text(json.dumps(commands, indent=2) + "\n")
        (args.output / "result.json").write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n")
        if unexpected: raise AssertionError(result["cleanup_error"])


if __name__ == "__main__":
    main()
