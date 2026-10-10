#!/usr/bin/env python3
import signal
import json
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "bench"))
from fanout_support import stop_process, wait_for_listener
from signaling_stub import SignalingStub

IDLE_TIMEOUT_SECONDS = 20


def free_ports(count):
    held = [socket.socket() for _ in range(count)]
    for connection in held:
        connection.bind(("127.0.0.1", 0))
    ports = [connection.getsockname()[1] for connection in held]
    for connection in held:
        connection.close()
    return ports


def wait_closed(connection, started, limit):
    connection.settimeout(1)
    while time.monotonic() - started < limit:
        try:
            if not connection.recv(4096):
                return time.monotonic() - started
        except socket.timeout:
            continue
        except OSError:
            return time.monotonic() - started
    return None


def silent_connection_closed(rtsp_port, results):
    # 连接建立后不再发送任何数据，必须在空闲期限内被关闭。
    started = time.monotonic()
    with socket.create_connection(("127.0.0.1", rtsp_port), timeout=2) as connection:
        elapsed = wait_closed(connection, started, IDLE_TIMEOUT_SECONDS + 10)
    passed = elapsed is not None and IDLE_TIMEOUT_SECONDS - 1 <= elapsed <= IDLE_TIMEOUT_SECONDS + 4
    results["silent"] = (passed, f"silent connection closed after {elapsed}s")


def frozen_udp_publisher_released(ffmpeg, rtsp_port, http_port, signaling, results):
    stream_id = signaling.issue("publish")
    url = f"rtsp://127.0.0.1:{rtsp_port}/{stream_id}"
    publish = [ffmpeg, "-hide_banner", "-loglevel", "error", "-re", "-f", "lavfi", "-i", "testsrc2=size=320x240:rate=20",
               "-c:v", "libx264", "-preset", "ultrafast", "-g", "20", "-an", "-f", "rtsp", "-rtsp_transport", "udp"]
    frozen = subprocess.Popen([*publish, url], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    replacement = None
    try:
        time.sleep(3)
        # 冻结后 RTSP 控制连接仍然存活，只能依靠 UDP 媒体空闲释放输入登记。
        frozen.send_signal(signal.SIGSTOP)
        time.sleep(IDLE_TIMEOUT_SECONDS + 3)
        with urllib.request.urlopen(f"http://127.0.0.1:{http_port}/receivers", timeout=3) as response:
            assert all(entry["stream_id"] != stream_id for entry in json.load(response)["receivers"]), "idle UDP input remains registered"
        next_id = signaling.issue("publish")
        replacement = subprocess.Popen([*publish, f"rtsp://127.0.0.1:{rtsp_port}/{next_id}"],
                                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(3)
        # 播放连接不计时：新 generation 持续播放超过空闲期限。
        started = time.monotonic()
        token = signaling.issue("play", next_id)
        play_url = f"rtsp://127.0.0.1:{rtsp_port}/{next_id}/{token}"
        player = subprocess.run([ffmpeg, "-hide_banner", "-loglevel", "error", "-rtsp_transport", "tcp", "-i", play_url,
                                 "-t", str(IDLE_TIMEOUT_SECONDS + 4), "-f", "null", "-"], capture_output=True, timeout=60)
        played = time.monotonic() - started
        passed = player.returncode == 0 and played >= IDLE_TIMEOUT_SECONDS + 3 and replacement.poll() is None
        results["udp"] = (passed, f"replacement publisher played for {played:.1f}s exit={player.returncode}")
    finally:
        frozen.send_signal(signal.SIGCONT)
        stop_process(replacement)
        stop_process(frozen)


def main():
    server_bin, ffmpeg = sys.argv[1:3]
    ports = free_ports(3)
    results = {}
    with SignalingStub() as signaling, tempfile.TemporaryDirectory(prefix="media-input-idle-") as temporary:
        log_path = Path(temporary) / "server.log"
        with log_path.open("w") as server_log:
            server = subprocess.Popen([server_bin, "--threads", "2", "--rtmp-port", str(ports[0]), "--rtsp-port", str(ports[1]),
                                       "--http-port", str(ports[2]), "--signaling-url", signaling.url], stdout=server_log, stderr=subprocess.STDOUT)
            try:
                wait_for_listener("127.0.0.1", ports[1])
                threads = [threading.Thread(target=silent_connection_closed, args=(ports[1], results)),
                           threading.Thread(target=frozen_udp_publisher_released, args=(ffmpeg, ports[1], ports[2], signaling, results))]
                for thread in threads:
                    thread.start()
                for thread in threads:
                    thread.join()
            finally:
                stop_process(server)
        for name in ("silent", "udp"):
            passed, message = results.get(name, (False, f"{name} did not finish"))
            print(("PASS " if passed else "FAIL ") + message)
        if not all(results.get(name, (False,))[0] for name in ("silent", "udp")):
            print(log_path.read_text())
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
