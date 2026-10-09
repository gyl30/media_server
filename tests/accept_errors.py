#!/usr/bin/env python3
import errno
import os
import socket
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "bench"))
from fanout_support import stop_process, wait_for_listener


def free_ports(count):
    held = [socket.socket() for _ in range(count)]
    for connection in held:
        connection.bind(("127.0.0.1", 0))
    ports = [connection.getsockname()[1] for connection in held]
    for connection in held:
        connection.close()
    return ports


def start(server_bin, fault_library, fault_errno, count):
    ports = free_ports(3)
    environment = {**os.environ, "LD_PRELOAD": fault_library, "ACCEPT_FAULT_ERRNO": str(fault_errno), "ACCEPT_FAULT_COUNT": str(count)}
    server = subprocess.Popen([server_bin, "--threads", "1", "--rtmp-port", str(ports[0]), "--rtsp-port", str(ports[1]),
                               "--http-port", str(ports[2])], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=environment)
    return server, ports


def rtsp_options(port):
    with socket.create_connection(("127.0.0.1", port), timeout=2) as connection:
        connection.sendall(b"OPTIONS rtsp://127.0.0.1/check RTSP/1.0\r\nCSeq: 1\r\n\r\n")
        connection.settimeout(2)
        return connection.recv(64).startswith(b"RTSP/1.0 200")


def main():
    server_bin, fault_library = sys.argv[1:3]

    # 单连接错误：监听继续工作。
    server, ports = start(server_bin, fault_library, errno.ECONNABORTED, 3)
    try:
        wait_for_listener("127.0.0.1", ports[1])
        recovered = any(rtsp_options(ports[1]) for _ in range(5))
    finally:
        stop_process(server)
    if not recovered:
        print("listener stopped after per-connection accept error")
        return 1

    # 监听 socket 不可用：服务以失败退出而不是空转重试。
    server, ports = start(server_bin, fault_library, errno.EINVAL, 1)
    try:
        for _ in range(20):
            try:
                socket.create_connection(("127.0.0.1", ports[1]), timeout=0.2).close()
            except OSError:
                pass
            if server.poll() is not None:
                break
            time.sleep(0.25)
        code = server.poll()
    finally:
        stop_process(server)
    if code is None or code == 0:
        print(f"fatal accept error did not fail the service: exit={code}")
        return 1
    print(f"per-connection error recovered; fatal error exit={code}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
