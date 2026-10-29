import asyncio
import json
import secrets
import socket
import struct
import subprocess
import sys
import time

import aiohttp

BIN = sys.argv[1]
STUB = sys.argv[2]
HTTP = "http://127.0.0.1:28080"


async def main():
    stub = subprocess.Popen([sys.executable, STUB])
    server = subprocess.Popen([BIN, "--rtmp-port", "21935", "--rtsp-port", "28554", "--http-port", "28080", "--bind-address", "127.0.0.1",
                               "--webrtc-address", "127.0.0.1", "--signaling-url", "http://127.0.0.1:29191"],
                              stdout=open(sys.argv[3], "w"), stderr=subprocess.STDOUT)
    rtp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rtp.bind(("127.0.0.1", 40000))
    rtp.setblocking(False)
    rtcp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rtcp.bind(("127.0.0.1", 40001))
    rtcp.setblocking(False)
    try:
        await asyncio.sleep(0.5)
        sid = secrets.token_hex(32)
        token = secrets.token_hex(32)
        headers = {"Content-Type": "application/json"}
        async with aiohttp.ClientSession() as http:
            async with http.post(f"{HTTP}/gb28181/audio/create", data=json.dumps({"session_id": sid}), headers=headers) as r:
                print("create", r.status, await r.text())
            body = {"session_id": sid, "upload_token": token, "remote_address": "127.0.0.1", "rtp_port": 40000, "rtcp_port": 40001,
                    "ssrc": 0x1234}
            async with http.post(f"{HTTP}/gb28181/audio/configure", data=json.dumps(body), headers=headers) as r:
                print("configure", r.status)
            ws = await http.ws_connect(f"ws://127.0.0.1:28080/publish/gb28181/audio/{sid}/{token}")
            packets = []
            reports = []
            start = time.monotonic()
            frame = 0
            while time.monotonic() - start < 12:
                await ws.send_bytes(bytes([(frame + i) & 0xff for i in range(160)]))
                frame += 1
                if frame % 50 == 0:
                    async with http.post(f"{HTTP}/gb28181/audio/status", data=json.dumps({"session_id": sid}), headers=headers) as r:
                        pass
                await asyncio.sleep(0.02)
                for sock, out in ((rtp, packets), (rtcp, reports)):
                    while True:
                        try:
                            out.append(sock.recv(2048))
                        except BlockingIOError:
                            break
            await ws.close()
            async with http.post(f"{HTTP}/gb28181/audio/delete", data=json.dumps({"session_id": sid}), headers=headers) as r:
                print("delete", r.status)
        seqs = [struct.unpack("!H", p[2:4])[0] for p in packets]
        ssrcs = {struct.unpack("!I", p[8:12])[0] for p in packets}
        gaps = sum(1 for a, b in zip(seqs, seqs[1:]) if (b - a) & 0xffff != 1)
        sizes = {len(p) for p in packets}
        types = [r[1] for r in reports]
        payload_ok = all(p[12:] == bytes([(i + k) & 0xff for k in range(160)]) for i, p in enumerate(packets))
        print(f"sent_frames={frame} rtp_packets={len(packets)} ssrcs={[hex(s) for s in ssrcs]} pt={set(p[1] & 0x7f for p in packets)} "
              f"gaps={gaps} sizes={sizes} payload_ok={payload_ok} rtcp_packets={len(reports)} rtcp_types={types}")
    finally:
        server.terminate()
        stub.terminate()
        print(f"server_exit={server.wait()}")
        stub.wait()


asyncio.run(main())
