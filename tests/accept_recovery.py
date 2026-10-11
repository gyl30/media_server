#!/usr/bin/env python3
import errno
import os
import resource
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "bench"))
from fanout_support import stop_process

STAGE_TIMEOUT = 5
POLL_INTERVAL = 0.01
SOCKET_TIMEOUT = 0.25


def limit_files():
    resource.setrlimit(resource.RLIMIT_NOFILE, (64, 64))


def rtsp_options(port):
    with socket.create_connection(("127.0.0.1", port), timeout=SOCKET_TIMEOUT) as connection:
        connection.sendall(b"OPTIONS rtsp://127.0.0.1/check RTSP/1.0\r\nCSeq: 1\r\n\r\n")
        return connection.recv(64).startswith(b"RTSP/1.0 200")


def main():
    server_bin = sys.argv[1]
    held = [socket.socket() for _ in range(3)]
    try:
        for connection in held:
            connection.bind(("127.0.0.1", 0))
        ports = [connection.getsockname()[1] for connection in held]
    finally:
        for connection in held:
            connection.close()

    with tempfile.NamedTemporaryFile(mode="w", prefix="media_server_accept_") as output:
        log_path = Path(output.name)
        server = subprocess.Popen([server_bin, "--threads", "1", "--rtmp-port", str(ports[0]), "--rtsp-port", str(ports[1]),
                                   "--http-port", str(ports[2])], stdout=output, stderr=subprocess.STDOUT,
                                  preexec_fn=limit_files)
        connections = []
        try:
            deadline = time.monotonic() + STAGE_TIMEOUT
            while time.monotonic() < deadline:
                if server.poll() is not None:
                    print(f"server exited during startup: {server.returncode}")
                    return 1
                try:
                    if rtsp_options(ports[1]):
                        break
                except OSError:
                    pass
                time.sleep(POLL_INTERVAL)
            else:
                print("server did not answer RTSP OPTIONS during startup")
                return 1

            emfile = f"accept failed port {ports[1]} error {os.strerror(errno.EMFILE)}"
            deadline = time.monotonic() + STAGE_TIMEOUT
            while time.monotonic() < deadline:
                if server.poll() is not None:
                    print(f"server exited before descriptor exhaustion was confirmed: {server.returncode}")
                    return 1
                if any(line.endswith(emfile) for line in log_path.read_text().splitlines()):
                    break
                if len(connections) < 200:
                    try:
                        connections.append(socket.create_connection(("127.0.0.1", ports[1]), timeout=SOCKET_TIMEOUT))
                    except OSError:
                        pass
                time.sleep(POLL_INTERVAL)
            else:
                print("server did not report EMFILE before the exhaustion deadline")
                return 1

            print(f"server reported EMFILE with {len(connections)} held connections")
            for connection in connections:
                connection.close()
            connections.clear()

            # accept 失败后固定 3 秒重试。
            deadline = time.monotonic() + STAGE_TIMEOUT
            while time.monotonic() < deadline:
                if server.poll() is not None:
                    print(f"server exited before accept recovered: {server.returncode}")
                    return 1
                try:
                    if rtsp_options(ports[1]) and server.poll() is None:
                        print("accept recovered; new RTSP OPTIONS returned 200")
                        return 0
                except OSError:
                    pass
                time.sleep(POLL_INTERVAL)
            print("listener did not recover after descriptor exhaustion")
            return 1
        finally:
            for connection in connections:
                connection.close()
            stop_process(server)
            print(log_path.read_text(), end="")


if __name__ == "__main__":
    sys.exit(main())
