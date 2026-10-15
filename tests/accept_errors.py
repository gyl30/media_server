#!/usr/bin/env python3
import errno
import os
import socket
import subprocess
import sys
import time
import tempfile
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


def start(server_bin, fault_library, fault_errno, skip, count, output):
    ports = free_ports(3)
    link_environment = dict(os.environ)
    link_environment.pop("LD_PRELOAD", None)
    libraries = subprocess.run(["ldd", server_bin], env=link_environment, capture_output=True,
                               text=True, check=True, timeout=10).stdout
    preload = [fault_library]
    for line in libraries.splitlines():
        fields = line.split()
        if fields and fields[0].startswith("libasan.so"):
            if len(fields) < 3 or fields[1] != "=>" or not Path(fields[2]).is_absolute() or not Path(fields[2]).is_file():
                raise RuntimeError(f"cannot resolve server ASan runtime: {line}")
            preload.insert(0, fields[2])
    environment = {**os.environ, "LD_PRELOAD": ":".join(preload), "ACCEPT_FAULT_ERRNO": str(fault_errno),
                   "ACCEPT_FAULT_SKIP": str(skip), "ACCEPT_FAULT_COUNT": str(count)}
    server = subprocess.Popen([server_bin, "--threads", str(THREADS), "--rtmp-port", str(ports[0]), "--rtsp-port", str(ports[1]),
                               "--http-port", str(ports[2])], stdout=output, stderr=subprocess.STDOUT, env=environment)
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
    with tempfile.TemporaryFile(mode="w+t") as output:
        server, port = start(server_bin, fault_library, errno.EINVAL, 1, 1, output)
        active = None
        try:
            if server.poll() is not None:
                raise RuntimeError(f"server exited during startup: {server.returncode}")
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
            output.seek(0)
            print(output.read(), end="")
            print(f"server exit status: {server.returncode}")
    if server.returncode != 0 or failed or not recovered or waited < 2.5 or not still_active or not alive:
        print(f"accept retry failed: failed={failed} recovered={recovered} waited={waited:.1f} active={still_active} alive={alive}")
        return 1
    print(f"accept error retried after {waited:.1f}s; active session kept")
    return 0


if __name__ == "__main__":
    sys.exit(main())
