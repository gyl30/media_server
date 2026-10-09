#!/usr/bin/env python3
import signal
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "bench"))
from fanout_support import stop_process, wait_for_listener

IDLE_TIMEOUT_SECONDS = 20


def free_ports(count):
    held = [socket.socket() for _ in range(count)]
    for connection in held:
        connection.bind(("127.0.0.1", 0))
    ports = [connection.getsockname()[1] for connection in held]
    for connection in held:
        connection.close()
    return ports


def control_messages_do_not_refresh(rtsp_port):
    # 只发送 RTSP 控制消息的连接没有媒体输入，必须在空闲期限内被关闭。
    started = time.monotonic()
    with socket.create_connection(("127.0.0.1", rtsp_port), timeout=2) as connection:
        sequence = 1
        while time.monotonic() - started < IDLE_TIMEOUT_SECONDS + 8:
            try:
                connection.sendall(f"OPTIONS rtsp://127.0.0.1/idle RTSP/1.0\r\nCSeq: {sequence}\r\n\r\n".encode())
                if not connection.recv(4096):
                    break
            except OSError:
                break
            sequence += 1
            time.sleep(2)
        elapsed = time.monotonic() - started
    if elapsed > IDLE_TIMEOUT_SECONDS + 4:
        print(f"control-only RTSP connection survived {elapsed:.1f}s")
        return False
    print(f"control-only RTSP connection closed after {elapsed:.1f}s")
    return True


def frozen_udp_publisher_released(ffmpeg, rtsp_port, log_path):
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
        time.sleep(4)
        if replacement.poll() is not None:
            print("frozen RTSP UDP publisher still owns the stream name")
            print(log_path.read_text())
            return False
        print("frozen RTSP UDP publisher released stream name")
        return True
    finally:
        frozen.send_signal(signal.SIGCONT)
        stop_process(replacement)
        stop_process(frozen)


def main():
    server_bin, ffmpeg = sys.argv[1:3]
    ports = free_ports(3)
    with tempfile.TemporaryDirectory(prefix="media-input-idle-") as temporary:
        log_path = Path(temporary) / "server.log"
        with log_path.open("w") as server_log:
            server = subprocess.Popen([server_bin, "--threads", "2", "--rtmp-port", str(ports[0]), "--rtsp-port", str(ports[1]),
                                       "--http-port", str(ports[2])], stdout=server_log, stderr=subprocess.STDOUT)
            try:
                wait_for_listener("127.0.0.1", ports[1])
                passed = control_messages_do_not_refresh(ports[1])
                passed = frozen_udp_publisher_released(ffmpeg, ports[1], log_path) and passed
            finally:
                stop_process(server)
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
