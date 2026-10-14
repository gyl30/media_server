#!/usr/bin/env python3
# 主动拉流的真实媒体验证：媒体服务器从上游拉流（RTMP / RTSP / WHEP 播放输出、FFmpeg 独立 RTMP 服务器），
# 再由独立 FFmpeg 从本服务器的 RTMP 输出解码音视频；上游结束后拉流会话随之结束。
import json
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
import uuid
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
                wait_until(lambda: receivers(base).get(pulled), 10, log)
                token = signaling.issue("play", pulled)
                decoded = subprocess.run([ffmpeg, "-nostdin", "-hide_banner", "-loglevel", "info", "-rw_timeout", "3000000",
                                          "-i", f"rtmp://127.0.0.1:{rtmp_port}/{pulled}/{token}", "-map", "0:v:0", "-map", "0:a:0",
                                          "-t", "2", "-f", "null", "-"], capture_output=True, text=True, timeout=15)
                assert decoded.returncode == 0, decoded.stderr
                assert "Video: h264" in decoded.stderr and "Audio: aac" in decoded.stderr and "time=00:00:02" in decoded.stderr, decoded.stderr
                print(f"{mode} pull -> RTMP output H.264/AAC decode: PASS", flush=True)

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
                text = log()
                assert server.returncode == 0 and "ERROR: AddressSanitizer" not in text and "runtime error:" not in text, text


if __name__ == "__main__":
    main()
