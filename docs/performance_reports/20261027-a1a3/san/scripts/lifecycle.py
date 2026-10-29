#!/usr/bin/env python3
"""Lifecycle and churn workload against one media_server process.

Phases: steady viewers, connect/abort churn and malformed connections,
publisher killed while viewers are attached, new stream_id lifecycle,
SIGTERM while publisher and viewers are active.
"""

import argparse
import asyncio
import json
import random
import re
import secrets
import signal
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path

HOST = "127.0.0.1"
RTMP_PORT, RTSP_PORT, HTTP_PORT, STUB_PORT = 21935, 28554, 28080, 29191
FFMPEG = str(Path.home() / "bin/ffmpeg")
PROTOCOLS = ("rtsp_tcp", "rtmp", "flv")
CHURN_PROTOCOLS = PROTOCOLS + ("rtsp_udp",)


def token():
    return secrets.token_hex(32)


def play_url(proto, stream, ticket):
    if proto.startswith("rtsp"):
        return f"rtsp://{HOST}:{RTSP_PORT}/{stream}/{ticket}"
    if proto == "rtmp":
        return f"rtmp://{HOST}:{RTMP_PORT}/{stream}/{ticket}"
    return f"http://{HOST}:{HTTP_PORT}/{stream}/{ticket}.flv"


class Workload:
    def __init__(self, args):
        self.args = args
        self.out = Path(args.out)
        self.results = {"phases": {}}
        self.publishers = []

    async def publish(self, stream):
        log = open(self.out / f"publish-{stream[:8]}.log", "w")
        process = await asyncio.create_subprocess_exec(
            FFMPEG, "-nostdin", "-v", "error", "-re", "-stream_loop", "-1", "-i", self.args.fixture, "-c", "copy", "-f", "flv",
            f"rtmp://{HOST}:{RTMP_PORT}/live/{stream}", stdout=log, stderr=subprocess.STDOUT)
        self.publishers.append(process)
        return process

    async def view(self, proto, stream, duration, kill_after=None):
        command = [FFMPEG, "-nostdin", "-v", "error"]
        if proto.startswith("rtsp"):
            command += ["-rtsp_transport", proto.split("_")[1]]
        command += ["-i", play_url(proto, stream, token()), "-t", str(duration), "-map", "0", "-f", "null", "-", "-progress", "pipe:1"]
        start = time.monotonic()
        process = await asyncio.create_subprocess_exec(*command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if kill_after is None:
            output, _ = await process.communicate()
        else:
            try:
                output, _ = await asyncio.wait_for(process.communicate(), kill_after)
            except asyncio.TimeoutError:
                try:
                    process.kill()
                except ProcessLookupError:
                    pass
                output, _ = await process.communicate()
        text = output.decode(errors="replace")
        frames = [int(value) for value in re.findall(r"^frame=(\d+)", text, re.M)]
        errors = [line for line in text.splitlines() if "=" not in line and line.strip()]
        return {"proto": proto, "frames": frames[-1] if frames else 0, "errors": errors[:3], "error_lines": len(errors),
                "rc": process.returncode, "elapsed": round(time.monotonic() - start, 2), "killed": kill_after is not None}

    async def abuse(self, index, stream):
        port, payload = random.choice([
            (RTMP_PORT, bytes([3]) + secrets.token_bytes(random.randint(1, 1536))),
            (RTMP_PORT, bytes([3]) + bytes(1536)),
            (RTMP_PORT, secrets.token_bytes(random.randint(1, 4096))),
            (RTSP_PORT, b"OPTIONS rtsp://127.0.0.1/ RTSP/1.0\r\nCSeq: 1\r\n"),
            (RTSP_PORT, f"DESCRIBE rtsp://{HOST}:{RTSP_PORT}/{stream}/{token()} RTSP/1.0\r\nCSeq: 2\r\n\r\n".encode()),
            (RTSP_PORT, b"SETUP rtsp://127.0.0.1/x/y/track1 RTSP/1.0\r\nCSeq: 3\r\nTransport: RTP/AVP/TCP;interleaved=0-1\r\n\r\n"),
            (RTSP_PORT, secrets.token_bytes(random.randint(1, 4096))),
            (HTTP_PORT, f"GET /{stream}/{token()}.flv HTTP/1.1\r\nHost: x\r\n\r\n".encode()),
            (HTTP_PORT, b"GET / HTTP/1.1\r\nHost: x\r\nX: " + b"a" * random.randint(1, 100_000)),
            (HTTP_PORT, f"POST /play/whep/{stream}/{token()} HTTP/1.1\r\nHost: x\r\nContent-Type: application/sdp\r\nContent-Length: 20\r\n\r\nv=0\r\no=bogus\r\ns=-\r\n".encode()),
            (HTTP_PORT, secrets.token_bytes(random.randint(1, 4096))),
        ])
        try:
            reader, writer = await asyncio.wait_for(asyncio.open_connection(HOST, port), 5)
        except (OSError, asyncio.TimeoutError):
            return "connect_failed"
        try:
            writer.write(payload)
            await writer.drain()
            await asyncio.wait_for(reader.read(random.choice([1, 4096])), random.uniform(0.0, 0.5))
        except (OSError, asyncio.TimeoutError, ConnectionError):
            pass
        sock = writer.get_extra_info("socket")
        if sock is not None and index % 2 == 0:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        writer.close()
        return "done"

    async def steady_and_churn(self, stream):
        steady = [asyncio.create_task(self.view(proto, stream, 16)) for proto in PROTOCOLS for _ in range(2)]
        semaphore = asyncio.Semaphore(10)

        async def churn_one(index):
            async with semaphore:
                proto = random.choice(CHURN_PROTOCOLS)
                if index % 3 == 0:
                    return await self.view(proto, stream, 30, kill_after=random.uniform(0.3, 2.0))
                return await self.view(proto, stream, random.choice([0.5, 1, 2]))

        abuse_semaphore = asyncio.Semaphore(20)

        async def abuse_one(index):
            async with abuse_semaphore:
                return await self.abuse(index, stream)

        churn = await asyncio.gather(*(churn_one(index) for index in range(self.args.churn)))
        abuse = await asyncio.gather(*(abuse_one(index) for index in range(self.args.abuse)))
        steady_results = await asyncio.gather(*steady)
        self.results["phases"]["steady"] = steady_results
        playable = [item for item in churn if item["proto"] != "rtsp_udp"]
        udp = [item for item in churn if item["proto"] == "rtsp_udp"]
        self.results["phases"]["churn"] = {
            "count": len(churn), "killed": sum(1 for item in playable if item["killed"]),
            "normal_total": sum(1 for item in playable if not item["killed"]),
            "normal_exit_with_frames": sum(1 for item in playable if not item["killed"] and item["rc"] == 0 and item["frames"] > 0),
            "rtsp_udp_total": len(udp),
            "rtsp_udp_rejected_461": sum(1 for item in udp if any("461" in line for line in item["errors"])),
            "sample_errors": [item for item in playable if not item["killed"] and (item["rc"] != 0 or item["frames"] == 0)][:5]}
        self.results["phases"]["abuse"] = {"count": len(abuse), "connect_failed": abuse.count("connect_failed")}

    async def source_end(self, stream, publisher):
        viewers = [asyncio.create_task(self.view(proto, stream, 60)) for proto in PROTOCOLS for _ in range(2)]
        await asyncio.sleep(5)
        killed_at = time.monotonic()
        publisher.kill()
        await publisher.wait()
        results = await asyncio.gather(*viewers)
        for item in results:
            item["exit_after_source_kill"] = round(item["elapsed"] - 5, 2)
        self.results["phases"]["source_end"] = results
        late = await self.view("flv", stream, 3)
        self.results["phases"]["play_ended_stream"] = late
        return killed_at

    async def run(self):
        stream_one = token()
        publisher = await self.publish(stream_one)
        await asyncio.sleep(2.5)
        await self.steady_and_churn(stream_one)
        await self.source_end(stream_one, publisher)

        stream_two = token()
        await self.publish(stream_two)
        await asyncio.sleep(2.5)
        self.results["phases"]["new_lifecycle"] = await asyncio.gather(*(self.view(proto, stream_two, 5) for proto in PROTOCOLS))
        tail = [asyncio.create_task(self.view(proto, stream_two, 60)) for proto in PROTOCOLS]
        await asyncio.sleep(4)
        return tail


async def wait_port(port, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            reader, writer = await asyncio.open_connection(HOST, port)
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
    parser.add_argument("--churn", type=int, default=60)
    parser.add_argument("--abuse", type=int, default=300)
    parser.add_argument("--shutdown-timeout", type=int, default=120)
    args = parser.parse_args()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    stub = subprocess.Popen([sys.executable, args.stub])
    server = subprocess.Popen([args.bin, "--rtmp-port", str(RTMP_PORT), "--rtsp-port", str(RTSP_PORT), "--http-port", str(HTTP_PORT),
                               "--bind-address", HOST, "--webrtc-address", HOST, "--threads", args.threads,
                               "--signaling-url", f"http://{HOST}:{STUB_PORT}"],
                              stdout=open(out / "server.log", "w"), stderr=subprocess.STDOUT)
    workload = Workload(args)
    results = workload.results
    try:
        if not await wait_port(HTTP_PORT, 60):
            raise RuntimeError("server did not listen")
        tail = await workload.run()
        results["server_alive_before_sigterm"] = server.poll() is None
        sigterm_at = time.monotonic()
        server.send_signal(signal.SIGTERM)
        try:
            results["server_exit_code"] = await asyncio.wait_for(asyncio.to_thread(server.wait), args.shutdown_timeout)
        except asyncio.TimeoutError:
            results["server_exit_code"] = "TIMEOUT"
            server.kill()
            server.wait()
        results["shutdown_seconds"] = round(time.monotonic() - sigterm_at, 2)
        tail_results = await asyncio.gather(*tail)
        results["phases"]["sigterm_viewers"] = [{"proto": item["proto"], "frames": item["frames"], "rc": item["rc"],
                                                 "exit_after_sigterm": round(item["elapsed"] - 4, 2)} for item in tail_results]
    finally:
        for process in workload.publishers:
            if process.returncode is None:
                process.kill()
                await process.wait()
        if server.poll() is None:
            server.kill()
            server.wait()
        stub.terminate()
        stub.wait()
        (out / "lifecycle.json").write_text(json.dumps(results, indent=1))
    print(json.dumps({"server_exit_code": results.get("server_exit_code"), "shutdown_seconds": results.get("shutdown_seconds"),
                      "steady": [(item["proto"], item["frames"], item["error_lines"]) for item in results["phases"].get("steady", [])],
                      "churn": {key: value for key, value in results["phases"].get("churn", {}).items() if key != "sample_errors"},
                      "source_end_exit": [(item["proto"], item["frames"], item["exit_after_source_kill"]) for item in results["phases"].get("source_end", [])],
                      "play_ended_stream": {key: results["phases"].get("play_ended_stream", {}).get(key) for key in ("frames", "rc", "elapsed")},
                      "new_lifecycle": [(item["proto"], item["frames"], item["error_lines"]) for item in results["phases"].get("new_lifecycle", [])],
                      "sigterm_viewers": results["phases"].get("sigterm_viewers")}))


asyncio.run(main())
