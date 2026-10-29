#!/usr/bin/env python3
"""Concurrent RTSP OPTIONS against one media_server; validates every Date header.

Each connection sends OPTIONS back to back so replies are formatted on all
workers at the same time. A Date header must match the RFC 822 layout used by
librtsp, carry a weekday consistent with its date, and fall within the
request/response wall-clock window.
"""

import argparse
import asyncio
import calendar
import json
import re
import signal
import subprocess
import sys
import time
from email.utils import parsedate_to_datetime
from pathlib import Path

HOST = "127.0.0.1"
RTMP_PORT, RTSP_PORT, HTTP_PORT, STUB_PORT = 21935, 28554, 28080, 29191
DATE = re.compile(r"^(Mon|Tue|Wed|Thu|Fri|Sat|Sun), (\d{2}) (Jan|Feb|Mar|Apr|May|Jun|Jul|Aug|Sep|Oct|Nov|Dec) (\d{4}) (\d{2}):(\d{2}):(\d{2}) GMT$")
WEEKDAYS = ("Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun")


async def client(index, deadline, stats):
    reader, writer = await asyncio.open_connection(HOST, RTSP_PORT)
    cseq = 0
    try:
        while time.monotonic() < deadline:
            cseq += 1
            before = time.time()
            writer.write(f"OPTIONS rtsp://{HOST}:{RTSP_PORT}/ RTSP/1.0\r\nCSeq: {cseq}\r\n\r\n".encode())
            await writer.drain()
            head = await reader.readuntil(b"\r\n\r\n")
            after = time.time()
            lines = head.decode(errors="replace").split("\r\n")
            stats["responses"] += 1
            if not lines[0].startswith("RTSP/1.0 200"):
                stats["non_200"] += 1
                stats["samples"].setdefault("non_200", lines[0])
            dates = [line[6:] for line in lines if line.startswith("Date: ")]
            if len(dates) != 1 or not DATE.match(dates[0]):
                stats["bad_format"] += 1
                stats["samples"].setdefault("bad_format", dates)
                continue
            value = dates[0]
            parsed = parsedate_to_datetime(value)
            if WEEKDAYS[parsed.weekday()] != value[:3]:
                stats["bad_weekday"] += 1
                stats["samples"].setdefault("bad_weekday", value)
            epoch = calendar.timegm(parsed.utctimetuple())
            if not int(before) - 1 <= epoch <= int(after) + 1:
                stats["out_of_window"] += 1
                stats["samples"].setdefault("out_of_window", [value, before, after])
            stats["samples"].setdefault("first", value)
            stats["last"] = value
    finally:
        writer.close()


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
    parser.add_argument("--out", required=True)
    parser.add_argument("--threads", default="4")
    parser.add_argument("--connections", type=int, default=32)
    parser.add_argument("--seconds", type=float, default=10)
    args = parser.parse_args()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    stub = subprocess.Popen([sys.executable, args.stub])
    server = subprocess.Popen([args.bin, "--rtmp-port", str(RTMP_PORT), "--rtsp-port", str(RTSP_PORT), "--http-port", str(HTTP_PORT),
                               "--bind-address", HOST, "--webrtc-address", HOST, "--threads", args.threads,
                               "--signaling-url", f"http://{HOST}:{STUB_PORT}"],
                              stdout=open(out / "server.log", "w"), stderr=subprocess.STDOUT)
    stats = {"responses": 0, "non_200": 0, "bad_format": 0, "bad_weekday": 0, "out_of_window": 0, "samples": {}, "last": None}
    try:
        if not await wait_port(RTSP_PORT, 60):
            raise RuntimeError("server did not listen")
        deadline = time.monotonic() + args.seconds
        await asyncio.gather(*(client(index, deadline, stats) for index in range(args.connections)))
        stats["server_alive"] = server.poll() is None
        server.send_signal(signal.SIGTERM)
        stats["server_exit_code"] = await asyncio.wait_for(asyncio.to_thread(server.wait), 120)
    finally:
        if server.poll() is None:
            server.kill()
            server.wait()
        stub.terminate()
        stub.wait()
        (out / "rtsp_date.json").write_text(json.dumps(stats, indent=1))
    print(json.dumps({key: stats[key] for key in ("responses", "non_200", "bad_format", "bad_weekday", "out_of_window", "server_alive", "server_exit_code")}
                     | {"first": stats["samples"].get("first"), "last": stats["last"]}))


asyncio.run(main())
