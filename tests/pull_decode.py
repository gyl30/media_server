#!/usr/bin/env python3
# 主动拉流的真实媒体验证：媒体服务器从上游拉流（RTMP / RTSP 播放输出、FFmpeg 独立 RTMP 服务器），
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
                           "rtsp": f"rtsp://127.0.0.1:{rtsp_port}/{source}/{play}"}[mode]

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
