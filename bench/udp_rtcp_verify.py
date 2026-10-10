#!/usr/bin/env python3
"""Verify real GB UDP sender/receiver reports across the existing RTCP interval."""

import argparse
import asyncio
import json
import select
import socket
import time
import uuid
from pathlib import Path

from fanout_support import benchmark_head, wait_for_stream
from lifecycle_verify import Run, http_wave, request


def reports(proxy, receiver):
    deadline = time.monotonic() + 33
    sender = None
    counts = {"sender_reports": 0, "receiver_reports": 0}
    while time.monotonic() < deadline:
        readable, _, _ = select.select([proxy], [], [], 0.2)
        if not readable:
            continue
        packet, remote = proxy.recvfrom(2048)
        assert len(packet) >= 8 and packet[0] >> 6 == 2, packet
        if remote == receiver:
            assert sender is not None and packet[1] == 201, (remote, packet[:8])
            counts["receiver_reports"] += 1
            target = sender
        else:
            assert packet[1] == 200 and (sender is None or sender == remote), (remote, packet[:8])
            sender = remote
            counts["sender_reports"] += 1
            target = receiver
        proxy.sendto(packet, target)
    assert counts["sender_reports"] > 0 and counts["receiver_reports"] > 0, counts
    return counts


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--client-dir", type=Path)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--ffmpeg", type=Path, default=Path("/home/gyl/bin/ffmpeg"))
    parser.add_argument("--port-base", type=int, default=26930)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.client_dir = args.client_dir or args.build_dir
    run = Run(args)
    result = {}
    try:
        with run.publisher("rtmp"), socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as proxy:
            proxy.bind((run.host, 0))
            proxy.setblocking(False)
            receiver = {"stream_id": str(uuid.uuid4()), "transport": "udp", "payload_type": 96, "ssrc": 1234}
            status, _, body = request(run.http_port, "POST", "/gb28181/receiver/create", receiver)
            assert status == 201, (status, body)
            port = json.loads(body)["rtp_port"]
            sender = {"stream_id": "live/perf0", "sender_id": "rtcp", "transport": "udp",
                      "payload_type": 96, "ssrc": 1234, "remote_address": run.host, "remote_rtp_port": port,
                      "remote_rtcp_port": proxy.getsockname()[1]}
            try:
                assert request(run.http_port, "POST", "/gb28181/sender/create", sender)[0] == 201
                wait_for_stream(run.host, run.http_port, receiver["stream_id"])
                clients = run.clients(receiver["stream_id"], duration=33, hls=False)

                async def receive():
                    return await asyncio.gather(asyncio.to_thread(reports, proxy, (run.host, port + 1)),
                                                http_wave(run.http_port, receiver["stream_id"], 1, 33))

                result["rtcp"], result["http_flv"] = asyncio.run(receive())
                result["media"] = run.finish(clients, 1)
            finally:
                for path, config in (("sender", sender), ("receiver", receiver)):
                    delete = {"stream_id": config["stream_id"]}
                    if path == "sender":
                        delete["sender_id"] = sender["sender_id"]
                    endpoint = "/receivers/delete" if path == "receiver" else "/gb28181/sender/delete"
                    assert request(run.http_port, "POST", endpoint, delete)[0] == 204
                    assert request(run.http_port, "POST", endpoint, delete)[0] == 404
            result["rtcp_interval_seconds"] = 25
            result["observation_seconds"] = 33
    finally:
        try:
            run.close()
        finally:
            (args.output / "commands.json").write_text(json.dumps(run.records, indent=2) + "\n")
    (args.output / "result.json").write_text(json.dumps({"head": benchmark_head(), "result": result}, indent=2) + "\n")
    print("GB UDP RTCP sender report + receiver report: PASS", flush=True)


if __name__ == "__main__":
    main()
