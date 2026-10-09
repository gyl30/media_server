#!/usr/bin/env python3
import errno
import os
import socket
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "bench"))
from fanout_support import stop_process

THREADS = 4


def free_ports(count):
    held = [socket.socket() for _ in range(count)]
    for connection in held:
        connection.bind(("127.0.0.1", 0))
    ports = [connection.getsockname()[1] for connection in held]
    for connection in held:
        connection.close()
    return ports


def start(server_bin, fault_library, fault_errno, skip, count):
    ports = free_ports(3)
    environment = {**os.environ, "LD_PRELOAD": fault_library, "ACCEPT_FAULT_ERRNO": str(fault_errno),
                   "ACCEPT_FAULT_SKIP": str(skip), "ACCEPT_FAULT_COUNT": str(count)}
    server = subprocess.Popen([server_bin, "--threads", str(THREADS), "--rtmp-port", str(ports[0]), "--rtsp-port", str(ports[1]),
                               "--http-port", str(ports[2])], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=environment)
    time.sleep(0.5)
    return server, ports[1]


def rtsp_options(connection, timeout=2):
    connection.sendall(b"OPTIONS rtsp://127.0.0.1/check RTSP/1.0\r\nCSeq: 1\r\n\r\n")
    connection.settimeout(timeout)
    return connection.recv(4096).startswith(b"RTSP/1.0 200")


def answered(port, timeout=2):
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=2) as connection:
            return rtsp_options(connection, timeout)
    except OSError:
        return False


def main():
    server_bin, fault_library = sys.argv[1:3]

    # accept 失败（含监听 socket 不可用类错误）不退出也不空转：已有会话继续服务，3 秒后重试接受新连接。
    server, port = start(server_bin, fault_library, errno.EINVAL, 1, 1)
    active = None
    try:
        active = socket.create_connection(("127.0.0.1", port), timeout=2)
        if not rtsp_options(active):
            print("active session was not served before the accept error")
            return 1
        failed = answered(port)
        began = time.monotonic()
        # 下一次 accept 在重试间隔之后才发生，连接期间停留在 backlog 中。
        recovered = answered(port, timeout=6)
        waited = time.monotonic() - began
        still_active = rtsp_options(active)
        alive = server.poll() is None
    finally:
        if active is not None:
            active.close()
        stop_process(server)
    if failed or not recovered or waited < 2.5 or not still_active or not alive:
        print(f"accept retry failed: failed={failed} recovered={recovered} waited={waited:.1f} active={still_active} alive={alive}")
        return 1
    print(f"accept error retried after {waited:.1f}s; active session kept")
    return 0


if __name__ == "__main__":
    sys.exit(main())
