"""Start a media_server with --threads N against a signaling stub and POST N WHIP offers at once; prints whether the server survived."""

import argparse
import os
import random
import signal
import socket
import subprocess
import sys
import threading
import time
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "bench"))
from signaling_stub import SignalingStub

OFFER = ("v=0\r\no=- 1000 2 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\na=group:BUNDLE 0 1\r\n"
         "m=video 9 UDP/TLS/RTP/SAVPF 102\r\nc=IN IP4 0.0.0.0\r\na=ice-ufrag:abcd\r\na=ice-pwd:abcdefghijklmnopqrstuvwx\r\n"
         "a=fingerprint:sha-256 " + ":".join(["AB"] * 32) + "\r\na=setup:actpass\r\na=mid:0\r\na=extmap:4 urn:ietf:params:rtp-hdrext:sdes:mid\r\na=sendonly\r\na=rtcp-mux\r\n"
         "a=rtpmap:102 H264/90000\r\na=fmtp:102 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f\r\n"
         "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\nc=IN IP4 0.0.0.0\r\na=ice-ufrag:abcd\r\na=ice-pwd:abcdefghijklmnopqrstuvwx\r\n"
         "a=fingerprint:sha-256 " + ":".join(["AB"] * 32) + "\r\na=setup:actpass\r\na=mid:1\r\na=extmap:4 urn:ietf:params:rtp-hdrext:sdes:mid\r\na=sendonly\r\na=rtcp-mux\r\n"
         "a=rtpmap:111 opus/48000/2\r\na=fmtp:111 minptime=10;useinbandfec=1\r\n")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True)
    parser.add_argument("--log", required=True)
    parser.add_argument("--threads", type=int, default=2)
    parser.add_argument("--count", type=int, default=3)
    parser.add_argument("--serial", action="store_true")
    args = parser.parse_args()
    base_port = random.randrange(20000, 60000, 10)
    with SignalingStub() as stub, open(args.log, "w") as log:
        server = subprocess.Popen([args.server, "--threads", str(args.threads), "--bind-address", "127.0.0.1", "--webrtc-address", "127.0.0.1",
                                   "--rtmp-port", str(base_port), "--rtsp-port", str(base_port + 1), "--http-port", str(base_port + 2),
                                   "--control-token", "x" * 32, "--signaling-url", stub.url], stdout=log, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            try:
                socket.create_connection(("127.0.0.1", base_port + 2), timeout=1).close()
                break
            except OSError:
                time.sleep(0.1)
        tokens = [stub.issue("publish") for _ in range(args.count)]
        barrier = threading.Barrier(args.count)
        statuses = {}

        def post(token):
            if not args.serial:
                barrier.wait()
            request = urllib.request.Request(f"http://127.0.0.1:{base_port + 2}/publish/whip/{token}", data=OFFER.encode(),
                                             headers={"Content-Type": "application/sdp"}, method="POST")
            try:
                with urllib.request.urlopen(request, timeout=30) as response:
                    statuses[token] = response.status
            except Exception as error:
                statuses[token] = f"{type(error).__name__}:{getattr(error, 'code', '')}:{error.read()[:80] if hasattr(error, 'read') else ''}"

        if args.serial:
            for token in tokens:
                post(token)
        else:
            workers = [threading.Thread(target=post, args=(token,)) for token in tokens]
            for worker in workers:
                worker.start()
            for worker in workers:
                worker.join()
        time.sleep(1)
        alive = server.poll() is None
        if alive:
            server.send_signal(signal.SIGTERM)
            try:
                server.wait(timeout=20)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait()
    print(f"alive={alive} rc={server.returncode} statuses={sorted(map(str, statuses.values()))}")


main()
