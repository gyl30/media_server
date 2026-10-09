#!/usr/bin/env python3
import socket
import subprocess
import sys
import urllib.error
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "bench"))
from fanout_support import stop_process, wait_for_listener


def status(url, token=None):
    request = urllib.request.Request(url)
    if token is not None:
        request.add_header("Authorization", f"Bearer {token}")
    try:
        with urllib.request.urlopen(request, timeout=3) as response:
            return response.status
    except urllib.error.HTTPError as error:
        return error.code


def run(server_bin, extra):
    held = [socket.socket() for _ in range(3)]
    for connection in held:
        connection.bind(("127.0.0.1", 0))
    ports = [connection.getsockname()[1] for connection in held]
    for connection in held:
        connection.close()
    server = subprocess.Popen([server_bin, "--threads", "1", "--rtmp-port", str(ports[0]), "--rtsp-port", str(ports[1]),
                               "--http-port", str(ports[2]), *extra], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        wait_for_listener("127.0.0.1", ports[2])
        return f"http://127.0.0.1:{ports[2]}", server
    except Exception:
        stop_process(server)
        raise


def main():
    server_bin = sys.argv[1]
    base, server = run(server_bin, ["--control-token", "secret"])
    try:
        results = {
            "no token": status(base + "/receivers"),
            "wrong token": status(base + "/receivers", "wrong"),
            "right token": status(base + "/receivers", "secret"),
            "play route": status(base + "/live/missing.flv"),
        }
    finally:
        stop_process(server)
    base, server = run(server_bin, [])
    try:
        results["loopback without token"] = status(base + "/receivers")
    finally:
        stop_process(server)
    expected = {"no token": 403, "wrong token": 403, "right token": 200, "play route": 404, "loopback without token": 200}
    print(results)
    return 0 if results == expected else 1


if __name__ == "__main__":
    sys.exit(main())
