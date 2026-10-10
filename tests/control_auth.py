#!/usr/bin/env python3
import json
import socket
import subprocess
import sys
import urllib.error
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "bench"))
from fanout_support import stop_process, wait_for_listener


def status(url, token=None, body=None):
    request = urllib.request.Request(url, json.dumps(body).encode() if body is not None else None)
    if body is not None:
        request.add_header("Content-Type", "application/json")
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
            "delete no token": status(base + "/receivers/delete", body={"stream_id": "missing"}),
            "delete wrong token": status(base + "/receivers/delete", "wrong", {"stream_id": "missing"}),
            "delete right token missing": status(base + "/receivers/delete", "secret", {"stream_id": "missing"}),
            "play route": status(base + "/" + "0" * 64 + "/" + "1" * 64 + ".flv"),
        }
        receiver = {"stream_id": "control-auth-receiver", "transport": "udp", "payload_type": 96, "ssrc": 1234}
        results["receiver create"] = status(base + "/gb28181/receiver/create", "secret", receiver)
        listing = urllib.request.Request(base + "/receivers", headers={"Authorization": "Bearer secret"})
        with urllib.request.urlopen(listing, timeout=3) as response:
            assert json.load(response) == {"receivers": [{"stream_id": receiver["stream_id"]}]}
        results["wrong id delete"] = status(base + "/receivers/delete", "secret", {"stream_id": "wrong-id"})
        results["receiver delete"] = status(base + "/receivers/delete", "secret", {"stream_id": receiver["stream_id"]})
        results["receiver repeat delete"] = status(base + "/receivers/delete", "secret", {"stream_id": receiver["stream_id"]})
        results["old gb delete route"] = status(base + "/gb28181/receiver/delete", "secret", {"stream_id": receiver["stream_id"]})
        results["old pull delete route"] = status(base + "/rtsp/pull/delete", "secret", {"stream_id": receiver["stream_id"]})
        with socket.socket() as upstream:
            upstream.bind(("127.0.0.1", 0))
            upstream.listen(1)
            pull = {"stream_id": "control-auth-pull", "url": f"rtsp://127.0.0.1:{upstream.getsockname()[1]}/source"}
            results["pull create"] = status(base + "/rtsp/pull/create", "secret", pull)
            with urllib.request.urlopen(listing, timeout=3) as response:
                assert json.load(response) == {"receivers": [{"stream_id": pull["stream_id"]}]}
            results["pull unified delete"] = status(base + "/receivers/delete", "secret", {"stream_id": pull["stream_id"]})
            results["pull repeat delete"] = status(base + "/receivers/delete", "secret", {"stream_id": pull["stream_id"]})
    finally:
        stop_process(server)
    base, server = run(server_bin, [])
    try:
        results["loopback without token"] = status(base + "/receivers")
        results["loopback delete without token"] = status(base + "/receivers/delete", body={"stream_id": "missing"})
    finally:
        stop_process(server)
    expected = {"no token": 403, "wrong token": 403, "right token": 200,
                "delete no token": 403, "delete wrong token": 403, "delete right token missing": 404,
                "receiver create": 201, "wrong id delete": 404, "receiver delete": 204,
                "receiver repeat delete": 404, "old gb delete route": 404, "old pull delete route": 404,
                "pull create": 201, "pull unified delete": 204, "pull repeat delete": 404,
                "play route": 404, "loopback without token": 200, "loopback delete without token": 404}
    print(results)
    return 0 if results == expected else 1


if __name__ == "__main__":
    sys.exit(main())
