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


def rtsp_options(connection):
    connection.sendall(b"OPTIONS rtsp://127.0.0.1/check RTSP/1.0\r\nCSeq: 1\r\n\r\n")
    connection.settimeout(2)
    return connection.recv(64).startswith(b"RTSP/1.0 200")


def answered(port):
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=2) as connection:
            return rtsp_options(connection)
    except OSError:
        return False


def main():
    server_bin, fault_library = sys.argv[1:3]

    # 单连接错误：被注入的 3 个连接失败，监听继续接受后续连接。
    # Asio 内部会吸收 ECONNABORTED/EPROTO，用 EPERM 才能经过应用层的立即继续分支。
    server, port = start(server_bin, fault_library, errno.EPERM, 0, 3)
    try:
        results = [answered(port) for _ in range(5)]
    finally:
        stop_process(server)
    if results != [False, False, False, True, True]:
        print(f"listener stopped after per-connection accept error: {results}")
        return 1

    # 监听 socket 不可用：在多 worker 且已有活跃会话时，服务以 1 退出而不是空转重试。
    server, port = start(server_bin, fault_library, errno.EINVAL, 1, 1)
    active = None
    try:
        active = socket.create_connection(("127.0.0.1", port), timeout=2)
        if not rtsp_options(active):
            print("active session was not served before the fatal error")
            return 1
        try:
            socket.create_connection(("127.0.0.1", port), timeout=1).close()
        except OSError:
            pass
        try:
            code = server.wait(timeout=10)
        except subprocess.TimeoutExpired:
            code = None
    finally:
        if active is not None:
            active.close()
        stop_process(server)
    if code != 1:
        print(f"fatal accept error did not fail the service with exit 1: exit={code}")
        return 1
    print(f"per-connection errors recovered {results}; fatal error with active session exit={code}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
