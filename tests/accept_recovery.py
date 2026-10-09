#!/usr/bin/env python3
import resource
import socket
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "bench"))
from fanout_support import stop_process, wait_for_listener


def limit_files():
    resource.setrlimit(resource.RLIMIT_NOFILE, (64, 64))


def rtsp_options(port):
    with socket.create_connection(("127.0.0.1", port), timeout=2) as connection:
        connection.sendall(b"OPTIONS rtsp://127.0.0.1/check RTSP/1.0\r\nCSeq: 1\r\n\r\n")
        connection.settimeout(2)
        return connection.recv(64).startswith(b"RTSP/1.0 200")


def main():
    server_bin = sys.argv[1]
    held = [socket.socket() for _ in range(3)]
    for connection in held:
        connection.bind(("127.0.0.1", 0))
    ports = [connection.getsockname()[1] for connection in held]
    for connection in held:
        connection.close()
    server = subprocess.Popen([server_bin, "--threads", "1", "--rtmp-port", str(ports[0]), "--rtsp-port", str(ports[1]),
                               "--http-port", str(ports[2])], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                              preexec_fn=limit_files)
    try:
        wait_for_listener("127.0.0.1", ports[1])
        connections = []
        exhausted = False
        for _ in range(200):
            try:
                connections.append(socket.create_connection(("127.0.0.1", ports[1]), timeout=2))
                time.sleep(0.005)
            except OSError:
                exhausted = True
                break
        # 服务端 fd 耗尽后 accept 失败；连接可能停留在 backlog 中而不报错。
        for connection in connections:
            connection.close()
        time.sleep(1)
        # accept 失败后固定 3 秒重试。
        for _ in range(40):
            try:
                if rtsp_options(ports[1]):
                    print(f"accept recovered after {len(connections)} connections exhausted={exhausted}")
                    return 0
            except OSError:
                pass
            time.sleep(0.25)
        print("listener did not recover after descriptor exhaustion")
        return 1
    finally:
        stop_process(server)


if __name__ == "__main__":
    sys.exit(main())
