#!/usr/bin/env python3
"""Capture UI density/state fixtures; real media lifecycle stays in gb_web_ui_verify.py."""

import argparse
import hashlib
import json
import subprocess
from pathlib import Path
from urllib.parse import urlsplit

from playwright.sync_api import sync_playwright

from fanout_support import stop_process, wait_for_listener


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--signaling", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--browser", default="/usr/bin/google-chrome")
    parser.add_argument("--port", type=int, default=43623)
    parser.add_argument("--review-only", action="store_true")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    result = {"source_sha256": {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in Path("signaling/web").glob("*")}, "scope": "browser presentation with API response fixtures; no media lifecycle claims", "screenshots": [], "checks": {}, "page_errors": []}
    with (args.output / "signaling.log").open("w") as log:
        process = subprocess.Popen([str(args.signaling), "--database", str(args.output / "devices.db"),
            "--http-listen", f"127.0.0.1:{args.port}", "--sip-listen", f"127.0.0.1:{args.port+1}"], stdout=log, stderr=subprocess.STDOUT)
    try:
        wait_for_listener("127.0.0.1", args.port)
        with sync_playwright() as playwright:
            browser = playwright.chromium.launch(executable_path=args.browser, headless=True)
            result["browser_version"] = browser.version
            page = browser.new_page(locale="zh-CN")
            page.on("pageerror", lambda error: result["page_errors"].append(str(error)))
            fixture = {"devices": [], "channels": [], "sources": [], "mode": "normal"}
            held = []

            def respond(route):
                path = urlsplit(route.request.url).path
                if fixture["mode"] == "network-error":
                    route.abort("failed")
                    return
                if fixture["mode"] == "loading" or (fixture["mode"] == "pending" and route.request.method in {"POST", "DELETE"}):
                    held.append(route)
                    return
                if route.request.method in {"POST", "DELETE"}:
                    route.fulfill(status=502, json={"error": "device_delete_failed" if route.request.method == "DELETE" else "live_start_failed"})
                    return
                if path.endswith("/channels"):
                    payload = {"channels": fixture["channels"]}
                elif path == "/api/devices":
                    payload = {"devices": fixture["devices"]}
                elif path.startswith("/api/devices/"):
                    payload = next(item for item in fixture["devices"] if item["device_id"] == path.rsplit("/", 1)[1])
                else:
                    payload = {"sources": fixture["sources"]}
                route.fulfill(json=payload)

            page.route("**/api/**", respond)

            def load(count=5, channels=8, online=True):
                fixture["mode"] = "normal"
                fixture["sources"] = []
                fixture["devices"] = [{"device_id": f"{34020000001320000001+i:020d}",
                    "name": "东区 · 二号楼一层出入口与公共区域高清摄像机" if i == 0 else f"{'大厅' if i%2 else '园区'} · 摄像机 {i+1:02d}",
                    "online": online if i == 0 else i%4 != 0} for i in range(count)]
                fixture["channels"] = [{"device_id": "34020000001320000001", "channel_id": f"{34020000001320000002+i:020d}",
                    "name": "二号楼 · 主入口与访客等候区域全景画面" if i == 0 else f"公共区域 · 通道 {i+1:02d}",
                    "status": "OFF" if i == 7 else "ON"} for i in range(channels)]
                page.goto(f"http://127.0.0.1:{args.port}/", wait_until="domcontentloaded")
                page.wait_for_function("document.querySelector('#last-updated').dateTime !== ''")
                if count:
                    page.locator(".device-item").first.click()
                    page.locator("#device-detail").wait_for()
                    if channels and online:
                        page.locator("#channel-rows tr").last.wait_for()

            def shot(name):
                page.screenshot(path=str(args.output / f"{name}.png"), full_page=True, animations="disabled")
                result["screenshots"].append(name)
                assert page.evaluate("document.documentElement.scrollWidth <= innerWidth"), name

            for width, height in ((1440, 900), (1920, 1080), (1000, 850), (600, 900)):
                page.set_viewport_size({"width": width, "height": height})
                for count in (0, 1, 5, 20):
                    load(count, 1 if count == 1 else 8)
                    shot(f"{width}-devices-{count}")
                    if count:
                        result["checks"][f"channel_action_visible_{width}"] = page.locator("#channel-rows [data-action=play]").first.evaluate("button => { const rect = button.getBoundingClientRect(); return rect.left >= 0 && rect.right <= innerWidth; }")
            page.set_viewport_size({"width": 1440, "height": 900})
            load(20)
            page.locator(".device-item").last.focus()
            page.locator(".device-item").last.scroll_into_view_if_needed()
            page.wait_for_timeout(3200)
            result["checks"]["scrolled_device_focus_survives_poll"] = page.evaluate("document.activeElement.dataset.deviceId === '34020000001320000020' && document.querySelector('#device-list').scrollTop > 0")
            ax = page.context.new_cdp_session(page).send("Accessibility.getFullAXTree")
            result["checks"]["tabpanel_in_accessibility_tree"] = any(node.get("role", {}).get("value") == "tabpanel" and not node.get("ignored") for node in ax["nodes"])
            load(5, 0, False)
            shot("offline")
            load(1, 0)
            shot("catalog-loading")
            page.wait_for_timeout(8000)
            shot("channels-empty")
            load()
            page.locator("#add-device-button").focus()
            page.keyboard.press("Enter")
            page.wait_for_timeout(3000)
            shot("device-dialog")
            device_tabs = []
            for _ in range(7):
                page.keyboard.press("Tab")
                device_tabs.append(page.evaluate("({id:document.activeElement.id, focused:document.hasFocus(), inside:document.querySelector('#device-dialog').contains(document.activeElement)})"))
            result["device_tab_sequence"] = device_tabs
            result["checks"]["device_dialog_tab_order_and_inert_background"] = (
                [item["id"] for item in device_tabs if item["inside"]][:5] == ["device-name", "cancel-device-dialog", "save-device-button", "close-device-dialog", "device-id"]
                and all(not item["focused"] or item["inside"] for item in device_tabs))
            page.keyboard.press("Escape")
            result["checks"]["native_dialog_returns_focus"] = page.evaluate("document.activeElement.id === 'add-device-button'")
            page.locator(".device-item").first.focus()
            page.wait_for_timeout(3200)
            result["checks"]["device_focus_survives_poll"] = page.evaluate("document.activeElement.matches('.device-item')")
            page.locator("#channel-rows [data-action=play]").first.focus()
            page.wait_for_timeout(3200)
            result["checks"]["channel_focus_survives_poll"] = page.evaluate("document.activeElement.matches('#channel-rows [data-action=play]')")
            shot("keyboard-focus")
            page.locator("#delete-device-button").click()
            shot("delete-confirmation")
            page.locator("#confirm-dialog [value=cancel]").click()
            fixture["mode"] = "pending"
            page.locator("#delete-device-button").click()
            page.locator("#confirm-action-button").click()
            page.wait_for_function("document.querySelector('#delete-device-button').disabled")
            shot("delete-pending")
            fixture["mode"] = "normal"
            for route in held:
                route.fulfill(status=502, json={"error": "device_delete_failed"})
            held.clear()
            page.locator("#global-status").wait_for()
            shot("delete-error")
            page.locator("#tab-sources").focus()
            page.keyboard.press("Enter")
            shot("sources-empty")
            fixture["sources"] = [{"source_id": f"source-{i}", "stream_name": "北区 · 大厅与访客通道高清摄像机" if i == 0 else f"rtsp/camera-{i+1}",
                "url": f"rtsp://camera.example:554/campus/north/building-02/floor-01/entrance-{i+1}/live", "username": "", "desired_state": "running" if i%2 else "stopped",
                "session": {"state": "created"} if i == 1 else None} for i in range(5)]
            page.locator("#refresh-button").click()
            page.locator("#source-rows tr").last.wait_for()
            page.locator("#source-rows [data-action=start]").first.focus()
            page.wait_for_timeout(3200)
            result["checks"]["source_focus_survives_poll"] = page.evaluate("document.activeElement.matches('#source-rows [data-action=start]')")
            shot("sources-list")
            page.locator("#source-rows [data-action=edit]").first.focus()
            page.keyboard.press("Enter")
            page.wait_for_timeout(3200)
            shot("source-dialog")
            source_tabs = []
            for _ in range(10):
                page.keyboard.press("Tab")
                source_tabs.append(page.evaluate("({id:document.activeElement.id, focused:document.hasFocus(), inside:document.querySelector('#source-dialog').contains(document.activeElement)})"))
            result["source_tab_sequence"] = source_tabs
            result["checks"]["source_dialog_tab_order_and_inert_background"] = (
                [item["id"] for item in source_tabs if item["inside"]][:8] == ["source-url", "source-username", "source-password", "clear-password", "cancel-source-dialog", "save-source-button", "close-source-dialog", "source-stream-name"]
                and all(not item["focused"] or item["inside"] for item in source_tabs))
            page.keyboard.press("Escape")
            page.wait_for_function("document.activeElement.matches('#source-rows [data-action=edit][data-source-id=source-0]')", timeout=2000)
            result["checks"]["source_dialog_returns_to_refreshed_trigger"] = page.evaluate("document.activeElement.matches('#source-rows [data-action=edit][data-source-id=source-0]')")
            page.locator("#source-rows [data-action=delete]").first.focus()
            page.keyboard.press("Enter")
            page.wait_for_timeout(3200)
            page.keyboard.press("Escape")
            page.wait_for_function("document.activeElement.matches('#source-rows [data-action=delete][data-source-id=source-0]')", timeout=2000)
            result["checks"]["confirmation_returns_to_refreshed_trigger"] = page.evaluate("document.activeElement.matches('#source-rows [data-action=delete][data-source-id=source-0]')")
            page.locator("#tab-sources").focus()
            page.keyboard.press("ArrowLeft")
            result["checks"]["tab_keyboard_selection"] = page.locator("#tab-devices").get_attribute("aria-selected") == "true"
            fixture["mode"] = "network-error"
            page.locator("#refresh-button").click()
            page.locator("#global-status").wait_for()
            shot("network-error")
            page.reload(wait_until="domcontentloaded")
            page.locator("#global-status").wait_for()
            result["checks"]["initial_error_does_not_claim_empty"] = page.locator("#device-empty strong").inner_text() == "无法连接服务器"
            shot("initial-error")
            fixture["mode"] = "loading"
            page.reload(wait_until="domcontentloaded")
            page.wait_for_timeout(150)
            result["checks"]["initial_loading_does_not_claim_empty"] = page.locator("#device-empty strong").inner_text() == "正在加载设备"
            shot("initial-loading")
            fixture["mode"] = "normal"
            for route in held:
                respond(route)
            held.clear()
            page.wait_for_function("document.querySelector('#last-updated').dateTime !== ''")
            page.emulate_media(reduced_motion="reduce")
            page.locator("#add-device-button").click()
            result["checks"]["reduced_motion"] = page.evaluate("parseFloat(getComputedStyle(document.querySelector('dialog[open]')).animationDuration) <= .01")
            shot("reduced-motion-dialog")
            result["checks"]["dialog_accessible_name"] = all(page.locator(selector).get_attribute("aria-labelledby")
                for selector in ("#device-dialog", "#source-dialog", "#confirm-dialog"))
            browser.close()
        result["status"] = "OBSERVATION" if args.review_only else "PASS"
        if not args.review_only:
            assert all(result["checks"].values()), result["checks"]
            assert not result["page_errors"], result["page_errors"]
    except Exception as error:
        result["status"] = "FAIL"
        result["error"] = str(error)
        raise
    finally:
        stop_process(process)
        result["exit_code"] = process.returncode
        (args.output / "result.json").write_text(json.dumps(result, indent=2, ensure_ascii=False)+"\n")


if __name__ == "__main__":
    main()
