#!/usr/bin/env python3
"""Control API request validation and response format regression against a local media_server."""

import argparse
import http.client
import json
import socket
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "bench"))
from signaling_stub import SignalingStub

TOKEN = "0123456789abcdef" * 4


def free_port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def receiver(stream_id='"x"', payload_type="96", ssrc="1", extra=""):
    return f'{{"stream_id":{stream_id},"transport":"udp","payload_type":{payload_type},"ssrc":{ssrc}{extra}}}'


def audio_configure(extra="", rtp_port="5000", rtcp_port="5001", ssrc="3", address="127.0.0.1"):
    return (f'{{"session_id":"{TOKEN}","upload_token":"{TOKEN}","remote_address":"{address}",'
            f'"rtp_port":{rtp_port},"rtcp_port":{rtcp_port},"ssrc":{ssrc}{extra}}}')


CASES = [
    ("GET", "/receivers", None, 200, {"receivers": []}),
    ("POST", "/gb28181/receiver/create", receiver('"r1"'), 201, "rtp_port"),
    ("POST", "/gb28181/receiver/create", receiver('"r\\u0032"', "127", "4294967295"), 201, "rtp_port"),
    ("POST", "/gb28181/receiver/create", receiver('"q\\"\\\\\\u00e9"'), 201, "rtp_port"),
    ("POST", "/gb28181/receiver/create", receiver('"c\\u0001\\u0000\\u2028z"'), 201, "rtp_port"),
    ("GET", "/receivers", None, 200, {"receivers": [{"stream_id": "c\u0001\u0000\u2028z", "ready": False},
                                                     {"stream_id": "q\"\\é", "ready": False},
                                                     {"stream_id": "r1", "ready": False},
                                                     {"stream_id": "r2", "ready": False}]}),
    ("POST", "/gb28181/receiver/create", receiver('"r1"'), 500, None),
    ("POST", "/gb28181/receiver/create", receiver(payload_type="128"), 400, None),
    ("POST", "/gb28181/receiver/create", receiver(payload_type="-1"), 400, None),
    ("POST", "/gb28181/receiver/create", receiver(payload_type="96.0"), 400, None),
    ("POST", "/gb28181/receiver/create", receiver(payload_type="9.6e1"), 400, None),
    ("POST", "/gb28181/receiver/create", receiver(payload_type='"96"'), 400, None),
    ("POST", "/gb28181/receiver/create", receiver(payload_type="null"), 400, None),
    ("POST", "/gb28181/receiver/create", receiver(ssrc="4294967296"), 400, None),
    ("POST", "/gb28181/receiver/create", receiver(ssrc="18446744073709551615"), 400, None),
    ("POST", "/gb28181/receiver/create", receiver(ssrc="18446744073709551616"), 400, None),
    ("POST", "/gb28181/receiver/create", receiver(ssrc="1" + "0" * 400), 400, None),
    ("POST", "/gb28181/receiver/create", '{"stream_id":"x","transport":"udp","payload_type":96}', 400, None),
    ("POST", "/gb28181/receiver/create", receiver(extra=',"extra":1'), 400, None),
    ("POST", "/gb28181/receiver/create", receiver('""'), 400, None),
    ("POST", "/gb28181/receiver/create", receiver("null"), 400, None),
    ("POST", "/gb28181/receiver/create", receiver('["x"]'), 400, None),
    ("POST", "/gb28181/receiver/create", '{"stream_id":"x","transport":"sctp","payload_type":96,"ssrc":1}', 400, None),
    ("POST", "/gb28181/receiver/create", "[1,2]", 400, None),
    ("POST", "/gb28181/receiver/create", "null", 400, None),
    ("POST", "/gb28181/receiver/create", "{", 400, None),
    ("POST", "/gb28181/receiver/create", "{} x", 400, None),
    ("POST", "/gb28181/receiver/create", "", 400, None),
    ("POST", "/gb28181/receiver/create", "[" * 2000 + "]" * 2000, 400, None),
    ("POST", "/gb28181/receiver/create", receiver('"\\ud800"'), 400, None),
    ("POST", "/gb28181/receiver/create", b'{"stream_id":"\xff","transport":"udp","payload_type":96,"ssrc":1}', 400, None),
    ("POST", "/gb28181/receiver/create", '{"stream_id":"x\x01","transport":"udp","payload_type":96,"ssrc":1}', 400, None),
    ("POST", "/gb28181/receiver/create", receiver(), 415, None, "text/plain"),
    ("POST", "/gb28181/receiver/create", '{"stream_id":"d1","stream_id":"d2","transport":"udp","payload_type":96,"ssrc":1}', 400, None),
    ("POST", "/gb28181/receiver/create", '{"stream_id":"d1","transport":"udp","payload_type":96,"ssrc":1,"ssrc":2}', 400, None),
    ("POST", "/gb28181/receiver/create",
     '{"stream_id":"x","transport":"tcp_active","remote_address":"0.0.0.0","remote_port":9,"payload_type":96,"ssrc":1}', 400, None),
    ("POST", "/gb28181/receiver/create",
     '{"stream_id":"x","transport":"tcp_active","remote_address":"127.0.0.1","remote_port":65536,"payload_type":96,"ssrc":1}', 400, None),
    ("POST", "/gb28181/receiver/create",
     '{"stream_id":"x","transport":"tcp_passive","listen_port":"9","payload_type":96,"ssrc":1}', 400, None),
    ("POST", "/gb28181/receiver/playback/create", receiver(), 400, None),
    ("POST", "/gb28181/receiver/update", '{"stream_id":"r1","ssrc":7}', 204, None),
    ("POST", "/gb28181/receiver/update", '{"stream_id":"r1","ssrc":7,"x":1}', 400, None),
    ("POST", "/gb28181/receiver/update", '{"stream_id":"r1","ssrc":7,"ssrc":8}', 400, None),
    ("POST", "/gb28181/receiver/update", '{"stream_id":"nope","ssrc":7}', 404, None),
    ("POST", "/gb28181/sender/create",
     '{"stream_id":"r1","sender_id":"s1","transport":"udp","remote_address":"127.0.0.1","remote_rtp_port":9000,'
     '"remote_rtcp_port":9000,"payload_type":96,"ssrc":1}', 400, None),
    ("POST", "/gb28181/sender/create",
     '{"stream_id":"r1","sender_id":"s1","sender_id":"s2","transport":"udp","remote_address":"127.0.0.1",'
     '"remote_rtp_port":9000,"payload_type":96,"ssrc":1}', 400, None),
    ("POST", "/gb28181/sender/delete", '{"stream_id":"r1","sender_id":"s1"}', 404, None),
    ("POST", "/gb28181/sender/delete", '{"stream_id":"r1","sender_id":"s1","sender_id":"s1"}', 400, None),
    ("POST", "/gb28181/sender/delete", '{"stream_id":"r1"}', 400, None),
    ("POST", "/pull/create", '{"stream_id":"p1","url":"rtsp://127.0.0.1:1/x","username":1}', 400, None),
    ("POST", "/pull/create", '{"stream_id":"p1","url":"rtsp://127.0.0.1:1/x","token":"t"}', 400, None),
    ("POST", "/pull/create", '{"stream_id":"p1","url":"rtsp://127.0.0.1:1/x","password":"p"}', 400, None),
    ("POST", "/pull/create", '{"stream_id":"p1","url":"rtmp://127.0.0.1:1/live/x","username":"u"}', 400, None),
    ("POST", "/pull/create", '{"stream_id":"p1","url":"ftp://127.0.0.1/x"}', 400, None),
    ("POST", "/pull/create", '{"stream_id":"p1","url":"rtsp://127.0.0.1:1/x","extra":true}', 400, None),
    ("POST", "/pull/create", '{"stream_id":"p1","url":"rtsp://u:p@127.0.0.1:1/x"}', 400, None),
    ("POST", "/pull/create", '{"stream_id":"p1","url":"rtmp://127.0.0.1:1/live/x","url":"rtsp://127.0.0.1:1/x"}', 400, None),
    ("POST", "/receivers/delete", '{"stream_id":"missing"}', 404, None),
    ("POST", "/receivers/delete", '{"stream_id":""}', 400, None),
    ("POST", "/receivers/delete", '{"stream_id":"r1","x":1}', 400, None),
    ("POST", "/receivers/delete", '{"stream_id":"r1","stream_id":"r2"}', 400, None),
    ("POST", "/receivers/delete", '{"stream_id":1}', 400, None),
    ("POST", "/receivers/delete", '{"stream_id":"r1"}', 204, None),
    ("POST", "/gb28181/audio/create", f'{{"session_id":"{TOKEN}"}}', 201, "rtp_port"),
    ("POST", "/gb28181/audio/create", f'{{"session_id":"{TOKEN}"}}', 409, None),
    ("POST", "/gb28181/audio/create", '{"session_id":"xyz"}', 400, None),
    ("POST", "/gb28181/audio/create", f'{{"session_id":"{TOKEN}","session_id":"{TOKEN}"}}', 400, None),
    ("POST", "/gb28181/audio/configure", audio_configure(rtp_port="0"), 400, None),
    ("POST", "/gb28181/audio/configure", audio_configure(rtp_port="1", rtcp_port="1"), 400, None),
    ("POST", "/gb28181/audio/configure", audio_configure(rtcp_port="-2"), 400, None),
    ("POST", "/gb28181/audio/configure", audio_configure(address="::1"), 400, None),
    ("POST", "/gb28181/audio/configure", audio_configure(extra=',"x":1'), 400, None),
    ("POST", "/gb28181/audio/configure", audio_configure(extra=',"ssrc":4'), 400, None),
    ("POST", "/gb28181/audio/status", f'{{"session_id":"{TOKEN}"}}', 204, None),
    ("POST", "/gb28181/audio/delete", f'{{"session_id":"{TOKEN}","x":1}}', 400, None),
    ("POST", "/gb28181/audio/delete", f'{{"session_id":"{TOKEN}"}}', 204, None),
    ("GET", "/receivers", None, 200, {"receivers": [{"stream_id": "c\u0001\u0000\u2028z", "ready": False},
                                                     {"stream_id": "q\"\\é", "ready": False},
                                                     {"stream_id": "r2", "ready": False}]}),
]


def check(connection_port, case):
    method, path, body, expected_status, expected_body = case[:5]
    content_type = case[5] if len(case) > 5 else "application/json"
    connection = http.client.HTTPConnection("127.0.0.1", connection_port, timeout=10)
    headers = {"Content-Type": content_type} if body is not None else {}
    connection.request(method, path, body=body, headers=headers)
    response = connection.getresponse()
    data = response.read()
    connection.close()
    problems = []
    if response.status != expected_status:
        problems.append(f"status {response.status} != {expected_status}")
    if data:
        if response.getheader("Content-Type", "").split(";")[0].strip().lower() != "application/json":
            problems.append("non-json content type")
        try:
            document = json.loads(data)
        except ValueError:
            problems.append(f"invalid json body {data!r}")
            document = None
        if document is not None:
            if expected_status >= 400 and set(document) != {"error"}:
                problems.append(f"error body {document}")
            if expected_body == "rtp_port":
                if set(document) != {"rtp_port"} or not isinstance(document["rtp_port"], int) or not 0 < document["rtp_port"] < 65536:
                    problems.append(f"create body {document}")
            elif isinstance(expected_body, dict):
                if path == "/receivers":
                    document["receivers"].sort(key=lambda item: item["stream_id"])
                if document != expected_body:
                    problems.append(f"body {document} != {expected_body}")
    elif expected_status >= 400 or expected_body is not None:
        problems.append("missing body")
    return problems


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server-bin", type=Path, required=True)
    args = parser.parse_args()

    failures = 0
    with SignalingStub() as stub:
        http_port = free_port()
        server = subprocess.Popen([str(args.server_bin), "--threads", "2", "--http-port", str(http_port), "--rtmp-port", str(free_port()),
                                   "--rtsp-port", str(free_port()), "--signaling-url", stub.url],
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            deadline = time.monotonic() + 10
            while True:
                try:
                    socket.create_connection(("127.0.0.1", http_port), timeout=1).close()
                    break
                except OSError:
                    if time.monotonic() > deadline or server.poll() is not None:
                        raise SystemExit("media_server did not start")
                    time.sleep(0.1)
            for case in CASES:
                problems = check(http_port, case)
                if problems:
                    failures += 1
                    print(f"FAIL {case[0]} {case[1]} {case[2]!r:.160}: {'; '.join(problems)}")
            if server.poll() is not None:
                failures += 1
                print(f"FAIL media_server exited with {server.returncode}")
        finally:
            server.terminate()
            exit_code = server.wait(10)
    if exit_code != 0:
        failures += 1
        print(f"FAIL media_server exit code {exit_code}")
    print(f"{len(CASES) - failures}/{len(CASES)} passed" if not failures else f"{failures} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
