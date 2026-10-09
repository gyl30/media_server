#!/usr/bin/env python3
import re
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "bench"))
from fanout_support import stop_process, wait_for_listener

IDLE_TIMEOUT_SECONDS = 20
SDP = ("v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\ns=idle\r\nc=IN IP4 127.0.0.1\r\nt=0 0\r\n"
       "m=video 0 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\n"
       "a=fmtp:96 packetization-mode=1;sprop-parameter-sets=Z0IAH5WoFAFuQA==,aM4G4g==\r\n"
       "a=control:trackID=1\r\n")


def free_ports(count):
    held = [socket.socket() for _ in range(count)]
    for connection in held:
        connection.bind(("127.0.0.1", 0))
    ports = [connection.getsockname()[1] for connection in held]
    for connection in held:
        connection.close()
    return ports


def rtsp_request(connection, text):
    connection.sendall(text.encode())
    response = b""
    while b"\r\n\r\n" not in response:
        chunk = connection.recv(4096)
        if not chunk:
            raise OSError("connection closed")
        response += chunk
    return response.decode(errors="replace")


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


def control_messages_do_not_refresh(rtsp_port, results):
    # 只发送 RTSP 控制消息的连接没有媒体输入，必须在空闲期限内被关闭。
    started = time.monotonic()
    with socket.create_connection(("127.0.0.1", rtsp_port), timeout=2) as connection:
        sequence = 1
        while time.monotonic() - started < IDLE_TIMEOUT_SECONDS + 8:
            try:
                rtsp_request(connection, f"OPTIONS rtsp://127.0.0.1/idle RTSP/1.0\r\nCSeq: {sequence}\r\n\r\n")
            except OSError:
                break
            sequence += 1
            time.sleep(2)
        elapsed = time.monotonic() - started
    passed = elapsed <= IDLE_TIMEOUT_SECONDS + 4
    results["control"] = (passed, f"control-only connection closed after {elapsed:.1f}s")


def late_setup_does_not_reset(rtsp_port, results):
    # 期限从连接建立开始：迟到的 SETUP/RECORD 不能重置期限。
    started = time.monotonic()
    url = f"rtsp://127.0.0.1:{rtsp_port}/idle/late"
    with socket.create_connection(("127.0.0.1", rtsp_port), timeout=2) as connection:
        rtsp_request(connection, f"ANNOUNCE {url} RTSP/1.0\r\nCSeq: 1\r\nContent-Type: application/sdp\r\n"
                                 f"Content-Length: {len(SDP)}\r\n\r\n{SDP}")
        time.sleep(15)
        response = rtsp_request(connection, f"SETUP {url}/trackID=1 RTSP/1.0\r\nCSeq: 2\r\n"
                                            "Transport: RTP/AVP/TCP;unicast;interleaved=0-1;mode=record\r\n\r\n")
        session = re.search(r"Session:\s*([^;\r\n]+)", response).group(1)
        rtsp_request(connection, f"RECORD {url} RTSP/1.0\r\nCSeq: 3\r\nSession: {session}\r\n\r\n")
        elapsed = wait_closed(connection, started, IDLE_TIMEOUT_SECONDS + 10)
    passed = elapsed is not None and elapsed <= IDLE_TIMEOUT_SECONDS + 4
    results["late_setup"] = (passed, f"late SETUP publisher closed after {elapsed}s")


def frozen_udp_publisher_released(ffmpeg, rtsp_port, results):
    url = f"rtsp://127.0.0.1:{rtsp_port}/idle/udp"
    publish = [ffmpeg, "-hide_banner", "-loglevel", "error", "-re", "-f", "lavfi", "-i", "testsrc2=size=320x240:rate=20",
               "-c:v", "libx264", "-preset", "ultrafast", "-g", "20", "-an", "-f", "rtsp", "-rtsp_transport", "udp", url]
    frozen = subprocess.Popen(publish, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    replacement = None
    try:
        time.sleep(3)
        # 冻结后 RTSP 控制连接仍然存活，只能依靠 UDP 媒体空闲释放流名。
        frozen.send_signal(signal.SIGSTOP)
        time.sleep(IDLE_TIMEOUT_SECONDS + 3)
        replacement = subprocess.Popen(publish, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(3)
        # 播放连接不计时：持续播放超过空闲期限，同时证明替代推流确实接管了流名。
        started = time.monotonic()
        player = subprocess.run([ffmpeg, "-hide_banner", "-loglevel", "error", "-rtsp_transport", "tcp", "-i", url,
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
    with tempfile.TemporaryDirectory(prefix="media-input-idle-") as temporary:
        log_path = Path(temporary) / "server.log"
        with log_path.open("w") as server_log:
            server = subprocess.Popen([server_bin, "--threads", "2", "--rtmp-port", str(ports[0]), "--rtsp-port", str(ports[1]),
                                       "--http-port", str(ports[2])], stdout=server_log, stderr=subprocess.STDOUT)
            try:
                wait_for_listener("127.0.0.1", ports[1])
                threads = [threading.Thread(target=control_messages_do_not_refresh, args=(ports[1], results)),
                           threading.Thread(target=late_setup_does_not_reset, args=(ports[1], results)),
                           threading.Thread(target=frozen_udp_publisher_released, args=(ffmpeg, ports[1], results))]
                for thread in threads:
                    thread.start()
                for thread in threads:
                    thread.join()
            finally:
                stop_process(server)
        for name in ("control", "late_setup", "udp"):
            passed, message = results.get(name, (False, f"{name} did not finish"))
            print(("PASS " if passed else "FAIL ") + message)
        if not all(results.get(name, (False,))[0] for name in ("control", "late_setup", "udp")):
            print(log_path.read_text())
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
