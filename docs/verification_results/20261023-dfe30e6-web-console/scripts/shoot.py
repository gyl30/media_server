#!/usr/bin/env python3
"""Screenshot console pages at several viewport sizes; prints console errors."""
import argparse
import asyncio
from pathlib import Path
from playwright.async_api import async_playwright

SIZES = {"w1440": (1440, 900), "w1280": (1280, 800), "w1024": (1024, 768), "w768": (768, 1024), "w390": (390, 844)}


async def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--base", default="http://127.0.0.1:39090/")
    parser.add_argument("--out", required=True)
    parser.add_argument("--sizes", default="w1440")
    parser.add_argument("--wait", type=float, default=1.5)
    parser.add_argument("--full", action="store_true")
    parser.add_argument("--webroot", default="")
    parser.add_argument("hashes", nargs="+")
    args = parser.parse_args()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    async with async_playwright() as p:
        browser = await p.chromium.launch(executable_path="/usr/bin/google-chrome", headless=True,
                                          args=["--allow-loopback-in-peer-connection", "--autoplay-policy=no-user-gesture-required", "--disable-features=LocalNetworkAccessChecks"])
        context = await browser.new_context()
        if args.webroot:
            root = Path(args.webroot)
            types = {".js": "text/javascript", ".css": "text/css", ".html": "text/html", ".svg": "image/svg+xml"}

            async def serve(route):
                path = route.request.url.split("://", 1)[1].split("/", 1)[1].split("?")[0].split("#")[0]
                if path.startswith("api/"):
                    await route.continue_()
                    return
                file = root / (path or "index.html")
                await route.fulfill(status=200, body=file.read_bytes(), headers={"Content-Type": types.get(file.suffix, "application/octet-stream")})

            await context.route("http://127.0.0.1:39090/**", serve)
        for size in args.sizes.split(","):
            width, height = SIZES[size]
            page = await context.new_page()
            await page.set_viewport_size({"width": width, "height": height})
            page.on("console", lambda m: m.type in ("error", "warning") and print("console", m.type, m.text))
            page.on("pageerror", lambda e: print("pageerror", e))
            for h in args.hashes:
                await page.goto(args.base + "#" + h)
                await page.wait_for_timeout(args.wait * 1000)
                name = (h or "root").replace("/", "_")
                await page.screenshot(path=str(out / f"{name}-{size}.png"), full_page=args.full)
                print("shot", out / f"{name}-{size}.png")
            await page.close()
        await context.close()
        await browser.close()

asyncio.run(main())
