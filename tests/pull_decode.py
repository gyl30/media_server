#!/usr/bin/env python3
# 主动拉流的真实媒体验证：媒体服务器从上游拉流（RTMP / RTSP / WHEP 播放输出、FFmpeg 独立 RTMP 服务器），
# 再由独立 FFmpeg 从本服务器的 RTMP 输出解码音视频；上游结束后拉流会话随之结束。
import json
import hashlib
import hmac
import re
import socket
import struct
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
import uuid
import threading
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "bench"))
from fanout_support import stop_process, wait_for_listener
from signaling_stub import SignalingStub


def free_ports(count):
    held = [socket.socket() for _ in range(count)]
    for connection in held:
        connection.bind(("127.0.0.1", 0))
    ports = [connection.getsockname()[1] for connection in held]
    for connection in held:
        connection.close()
    return ports


def post(base, path, body):
    request = urllib.request.Request(base + path, json.dumps(body).encode(), {"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(request, timeout=3) as response:
            return response.status
    except urllib.error.HTTPError as error:
        return error.code


def receivers(base):
    with urllib.request.urlopen(base + "/receivers", timeout=3) as response:
        return {item["stream_id"]: item["ready"] for item in json.load(response)["receivers"]}


def wait_until(condition, timeout, message):
    deadline = time.monotonic() + timeout
    while not condition():
        assert time.monotonic() < deadline, message()
        time.sleep(0.1)


class WhepProxy:
    """转发 WHEP 请求到真实端点，可改写应答以构造 DTLS 指纹不符或 candidate 不可达；记录收到的 DELETE。"""

    def __init__(self, upstream, rewrite, delay=0):
        import http.server
        import threading
        proxy = self
        self.deletes = []
        self.authorization = []

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_POST(self):
                proxy.authorization.append(self.headers.get("Authorization"))
                body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
                request = urllib.request.Request(upstream, body, {"Content-Type": "application/sdp"})
                with urllib.request.urlopen(request, timeout=3) as response:
                    answer = rewrite(response.read().decode())
                    location = response.headers["Location"]
                time.sleep(delay)
                self.send_response(201)
                self.send_header("Content-Type", "application/sdp")
                self.send_header("Location", "/resource" + location)
                self.send_header("Content-Length", str(len(answer)))
                self.end_headers()
                self.wfile.write(answer.encode())

            def do_DELETE(self):
                proxy.authorization.append(self.headers.get("Authorization"))
                proxy.deletes.append(self.path)
                self.send_response(200)
                self.send_header("Content-Length", "0")
                self.end_headers()

            def log_message(self, *_):
                pass

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.url = f"http://127.0.0.1:{self.server.server_port}/whep"

    def close(self):
        self.server.shutdown()
        self.server.server_close()


class WhepRelay:
    """真实 DTLS/SRTP 双向透传；仅改变成功 STUN 事务或停止其响应。"""

    def __init__(self):
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.socket.bind(("127.0.0.1", 0))
        self.socket.settimeout(0.1)
        self.upstream = None
        self.client = None
        self.password = None
        self.wrong_transaction = False
        self.drop_consent = False
        self.responses = 0
        self.altered = 0
        self.dropped = 0
        self.media = 0
        self.error = None
        self.closed = threading.Event()
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def rewrite(self, answer):
        candidate = re.search(r"a=candidate:\S+ 1 UDP \d+ (127\.0\.0\.1) (\d+)", answer)
        assert candidate, answer
        self.upstream = (candidate[1], int(candidate[2]))
        self.password = re.search(r"a=ice-pwd:([^\r\n]+)", answer)[1].encode()
        return answer[:candidate.start(2)] + str(self.socket.getsockname()[1]) + answer[candidate.end(2):]

    def alter_transaction(self, packet):
        packet = bytearray(packet)
        packet[19] ^= 0x80  # 8-byte prefix remains genuine; remaining transaction ID was never sent.
        offset = 20
        while offset < len(packet):
            kind, size = struct.unpack_from("!HH", packet, offset)
            if kind == 0x0008:
                prefix = bytearray(packet[:offset])
                struct.pack_into("!H", prefix, 2, offset + 24 - 20)
                packet[offset + 4:offset + 24] = hmac.new(self.password, prefix, hashlib.sha1).digest()
            elif kind == 0x8028:
                struct.pack_into("!I", packet, offset + 4, zlib.crc32(packet[:offset]) ^ 0x5354554E)
            offset += 4 + ((size + 3) & ~3)
        return bytes(packet)

    def run(self):
        try:
            while not self.closed.is_set():
                try:
                    packet, source = self.socket.recvfrom(65536)
                except socket.timeout:
                    continue
                if source == self.upstream:
                    if packet[:2] == b"\x01\x01" and packet[4:8] == b"\x21\x12\xa4\x42":
                        self.responses += 1
                        if self.drop_consent:
                            self.dropped += 1
                            continue
                        if self.wrong_transaction:
                            packet = self.alter_transaction(packet)
                            self.altered += 1
                    elif packet and 128 <= packet[0] <= 191:
                        self.media += 1
                    if self.client is not None:
                        self.socket.sendto(packet, self.client)
                else:
                    self.client = source
                    if self.upstream is not None:
                        self.socket.sendto(packet, self.upstream)
        except OSError as error:
            if not self.closed.is_set():
                self.error = error
        except Exception as error:
            self.error = error

    def close(self):
        self.closed.set()
        self.thread.join(timeout=1)
        self.socket.close()
        assert not self.thread.is_alive() and self.error is None, self.error


# 外部 WHEP 端点的失败路径：拒绝连接、HTTP 错误、非法应答、DTLS 指纹不符、candidate 不可达。
# 每种情况下拉流会话都会结束；已经创建的外部资源会被 DELETE。
def whep_failures(base, http_port, source, signaling, log):
    import re
    upstream = lambda: f"http://127.0.0.1:{http_port}/play/whep/{source}/{signaling.issue('play', source)}"
    cases = [("refused", f"http://127.0.0.1:{free_ports(1)[0]}/whep", None),
             ("not found", f"http://127.0.0.1:{http_port}/play/whep/{source}/{'0' * 64}", None)]
    proxies = []
    for name, rewrite in (("invalid sdp", lambda answer: "v=0\r\n"),
                          ("dtls fingerprint", lambda answer: re.sub(r"a=fingerprint:sha-256 \S+", "a=fingerprint:sha-256 " + ":".join(["AB"] * 32), answer)),
                          ("ice unreachable", lambda answer: re.sub(r"(a=candidate:\S+ 1 UDP \d+ 127\.0\.0\.1 )\d+", r"\g<1>9", answer))):
        target = upstream()
        proxy = WhepProxy(target, rewrite)
        proxies.append((name, proxy))
        cases.append((name, proxy.url, proxy))
    try:
        # 协商期间删除：迟到的 POST 结果不建立会话，刚创建的外部资源随即被 DELETE。
        proxy = WhepProxy(upstream(), lambda answer: answer, delay=1.5)
        proxies.append(("closed during negotiation", proxy))
        pulled = str(uuid.uuid4())
        assert post(base, "/pull/create", {"stream_id": pulled, "url": proxy.url, "token": "bearer-test"}) == 201
        assert post(base, "/receivers/delete", {"stream_id": pulled}) == 204
        wait_until(lambda: len(proxy.deletes) == 1, 8, lambda: "resource created during negotiation was not deleted")
        assert pulled not in receivers(base) and "whep pull ice connected " + pulled not in log(), log()
        print("WHEP pull closed during negotiation deletes late resource: PASS", flush=True)

        for name, url, proxy in cases:
            pulled = str(uuid.uuid4())
            started = time.monotonic()
            assert post(base, "/pull/create", {"stream_id": pulled, "url": url, "token": "bearer-test"}) == 201, name
            wait_until(lambda: pulled not in receivers(base), 25, log)
            if proxy is not None:
                wait_until(lambda: len(proxy.deletes) == 1, 5, lambda: f"{name}: resource not deleted")
                assert proxy.authorization == ["Bearer bearer-test"] * 2 and proxy.deletes[0].startswith("/resource/"), (proxy.authorization, proxy.deletes)
            print(f"WHEP pull failure '{name}' ends session in {time.monotonic() - started:.1f}s: PASS", flush=True)
    finally:
        for _, proxy in proxies:
            proxy.close()


def main():
    server_bin, ffmpeg, mode = sys.argv[1:4]
    with tempfile.TemporaryDirectory(prefix="media-pull-decode-") as temporary:
        output = Path(temporary)
        fixture = output / "fixture.flv"
        subprocess.run([ffmpeg, "-nostdin", "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i", "testsrc2=size=320x240:rate=20",
                        "-f", "lavfi", "-i", "sine=frequency=1000:sample_rate=44100", "-t", "3", "-c:v", "libx264", "-preset", "ultrafast",
                        "-tune", "zerolatency", "-g", "20", "-c:a", "aac", "-ac", "2", str(fixture)], check=True, timeout=15)
        rtmp_port, rtsp_port, http_port, listen_port = free_ports(4)
        base = f"http://127.0.0.1:{http_port}"
        with SignalingStub() as signaling, (output / "server.log").open("w") as server_log, (output / "upstream.log").open("w") as upstream_log:
            server = subprocess.Popen([server_bin, "--threads", "2", "--rtmp-port", str(rtmp_port), "--rtsp-port", str(rtsp_port),
                                       "--http-port", str(http_port), "--bind-address", "127.0.0.1", "--webrtc-address", "127.0.0.1",
                                       "--signaling-url", signaling.url], stdout=server_log, stderr=subprocess.STDOUT)
            upstream = None
            proxy = None
            relay = None
            log = lambda: (output / "server.log").read_text()
            try:
                wait_for_listener("127.0.0.1", http_port)
                loop = [ffmpeg, "-nostdin", "-hide_banner", "-loglevel", "error", "-re", "-stream_loop", "-1", "-i", str(fixture), "-c", "copy"]
                if mode in ("rtmp_listen", "rtmp_unsupported"):
                    codec = ["-c:v", "copy", "-c:a", "libmp3lame", "-ar", "44100"] if mode == "rtmp_unsupported" else []
                    upstream = subprocess.Popen([*loop, *codec, "-f", "flv", "-listen", "1", f"rtmp://127.0.0.1:{listen_port}/live/cam"],
                                                stdout=upstream_log, stderr=subprocess.STDOUT)
                    # FFmpeg 监听模式只接受一个连接，不能用探测连接等待，只等待其完成启动。
                    time.sleep(1.5)
                    url = f"rtmp://127.0.0.1:{listen_port}/live/cam"
                else:
                    source = signaling.issue("publish")
                    upstream = subprocess.Popen([*loop, "-f", "flv", f"rtmp://127.0.0.1:{rtmp_port}/live/{source}"],
                                                stdout=upstream_log, stderr=subprocess.STDOUT)
                    wait_until(lambda: receivers(base).get(source), 10, log)
                    play = signaling.issue("play", source)
                    url = {"rtmp": f"rtmp://127.0.0.1:{rtmp_port}/{source}/{play}",
                           "rtsp": f"rtsp://127.0.0.1:{rtsp_port}/{source}/{play}",
                           "whep": f"http://127.0.0.1:{http_port}/play/whep/{source}/{play}"}.get(mode, "")
                    if mode in ("whep_shutdown", "whep_transaction", "whep_consent"):
                        relay = WhepRelay()
                        relay.wrong_transaction = mode == "whep_transaction"
                        proxy = WhepProxy(f"http://127.0.0.1:{http_port}/play/whep/{source}/{play}", relay.rewrite)
                        url = proxy.url
                    elif mode == "whep_mixed_candidates":
                        def prepend_ipv6(answer):
                            offset = answer.find("a=candidate:")
                            assert offset >= 0 and " 127.0.0.1 " in answer[offset:], answer
                            return answer[:offset] + "a=candidate:mixed 1 UDP 2130706431 ::1 9 typ host\r\n" + answer[offset:]
                        proxy = WhepProxy(f"http://127.0.0.1:{http_port}/play/whep/{source}/{play}", prepend_ipv6)
                        url = proxy.url
                if mode == "whep_failures":
                    whep_failures(base, http_port, source, signaling, log)
                    return

                if mode == "rtmp_unsupported":
                    # MP3 音频不是内部支持的格式：拉流会话明确失败结束，不注册只含部分轨道的媒体源。
                    rejected = str(uuid.uuid4())
                    assert post(base, "/pull/create", {"stream_id": rejected, "url": url}) == 201, log()
                    wait_until(lambda: rejected not in receivers(base), 15, log)
                    assert "rtmp input unsupported codec" in log(), log()
                    print("unsupported RTMP codec ends pull session: PASS", flush=True)
                    return

                pulled = str(uuid.uuid4())
                assert post(base, "/pull/create", {"stream_id": pulled, "url": url}) == 201, log()
                assert post(base, "/pull/create", {"stream_id": pulled, "url": url}) == 409, log()
                if mode == "whep_transaction":
                    wait_until(lambda: relay.altered >= 1, 5, lambda: "no genuine STUN response was rewritten by relay")
                    time.sleep(0.5)  # Allow the rewritten authenticated response to reach the DUT's owner handler.
                    assert relay.error is None, relay.error
                    assert "whep pull ice connected " + pulled not in log() and "whep pull dtls connected " + pulled not in log(), \
                        "authenticated response with unsent transaction was accepted\n" + log()
                    assert not receivers(base).get(pulled), "unsent STUN transaction established media\n" + log()
                    relay.wrong_transaction = False
                wait_until(lambda: receivers(base).get(pulled), 10, log)
                token = signaling.issue("play", pulled)
                decoded = subprocess.run([ffmpeg, "-nostdin", "-hide_banner", "-loglevel", "info", "-rw_timeout", "3000000",
                                          "-i", f"rtmp://127.0.0.1:{rtmp_port}/{pulled}/{token}", "-map", "0:v:0", "-map", "0:a:0",
                                          "-t", "2", "-f", "null", "-"], capture_output=True, text=True, timeout=15)
                assert decoded.returncode == 0, decoded.stderr
                assert "Video: h264" in decoded.stderr and "Audio: aac" in decoded.stderr and "time=00:00:02" in decoded.stderr, decoded.stderr
                print(f"{mode} pull -> RTMP output H.264/AAC decode: PASS", flush=True)

                if mode == "whep_shutdown":
                    stop_process(server)
                    assert server.returncode == 0, log()
                    wait_until(lambda: len(proxy.deletes) == 1, 8, lambda: "worker stop lost external WHEP DELETE\n" + log())
                    assert relay.error is None, relay.error
                    print("WHEP worker stop drains external DELETE: PASS", flush=True)
                    return

                if mode == "whep_consent":
                    relay.drop_consent = True
                    media_before = relay.media
                    started = time.monotonic()
                    time.sleep(10)
                    assert receivers(base).get(pulled), "consent expired prematurely\n" + log()
                    # Verify authenticated video/audio still decode after responses have stopped.
                    token = signaling.issue("play", pulled)
                    decoded = subprocess.run([ffmpeg, "-nostdin", "-hide_banner", "-loglevel", "info", "-rw_timeout", "3000000",
                                              "-i", f"rtmp://127.0.0.1:{rtmp_port}/{pulled}/{token}", "-map", "0:v:0", "-map", "0:a:0",
                                              "-t", "2", "-f", "null", "-"], capture_output=True, text=True, timeout=15)
                    assert decoded.returncode == 0 and "time=00:00:02" in decoded.stderr, decoded.stderr
                    wait_until(lambda: pulled not in receivers(base), max(1, 36 - (time.monotonic() - started)),
                               lambda: "continuous authenticated media prevented consent expiry\n" + log())
                    assert 24 <= time.monotonic() - started <= 36, time.monotonic() - started
                    assert receivers(base).get(source) and relay.media - media_before > 200 and relay.dropped >= 4, (relay.media, relay.dropped, log())
                    wait_until(lambda: len(proxy.deletes) == 1, 5, lambda: "consent-expired resource not deleted")
                    print("WHEP consent expires despite continuous decodable SRTP: PASS", flush=True)
                    return

                stop_process(upstream)
                upstream = None
                wait_until(lambda: pulled not in receivers(base), 25, log)
                print(f"{mode} upstream end -> pull session released: PASS", flush=True)

                for invalid in ("rtmp://127.0.0.1/app-only", "rtmp://127.0.0.1//stream", "rtmp://user:pass@127.0.0.1/a/b", "ftp://127.0.0.1/a/b"):
                    assert post(base, "/pull/create", {"stream_id": str(uuid.uuid4()), "url": invalid}) == 400, invalid
                assert post(base, "/pull/create", {"stream_id": str(uuid.uuid4()), "url": "rtmp://127.0.0.1/a/b", "password": "x"}) == 400
                refused = str(uuid.uuid4())
                assert post(base, "/pull/create", {"stream_id": refused, "url": f"rtmp://127.0.0.1:{free_ports(1)[0]}/live/x"}) == 201
                wait_until(lambda: refused not in receivers(base), 10, log)
                print("invalid URL rejection and refused connection cleanup: PASS", flush=True)
            finally:
                stop_process(upstream)
                stop_process(server)
                if proxy is not None:
                    proxy.close()
                if relay is not None:
                    relay.close()
                text = log()
                assert server.returncode == 0 and "ERROR: AddressSanitizer" not in text and "runtime error:" not in text, text


if __name__ == "__main__":
    main()
