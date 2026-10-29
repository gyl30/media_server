#!/usr/bin/env python3
"""Concurrent /pull/create and /receivers/delete on one WHEP pull stream_id (control API without token)."""
import argparse
import http.client
import json
import threading
import time
import uuid


def post(host, port, path, body):
    connection = http.client.HTTPConnection(host, port, timeout=10)
    try:
        connection.request("POST", path, json.dumps(body), {"Content-Type": "application/json"})
        return connection.getresponse().status
    except OSError:
        return 0
    finally:
        connection.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True)
    parser.add_argument("--seconds", type=float, default=20)
    parser.add_argument("--pairs", type=int, default=4)
    args = parser.parse_args()
    host, port = args.server.split(":")
    deadline = time.monotonic() + args.seconds
    statuses = {}
    lock = threading.Lock()

    def count(kind, status):
        with lock:
            statuses[(kind, status)] = statuses.get((kind, status), 0) + 1

    def creator(stream):
        while time.monotonic() < deadline:
            count("create", post(host, int(port), "/pull/create", {"stream_id": stream, "url": f"http://127.0.0.1:1/play/whep/{stream}/{'0' * 64}"}))

    def deleter(stream):
        while time.monotonic() < deadline:
            count("delete", post(host, int(port), "/receivers/delete", {"stream_id": stream}))

    threads = []
    for _ in range(args.pairs):
        stream = str(uuid.uuid4())
        threads += [threading.Thread(target=creator, args=(stream,)), threading.Thread(target=deleter, args=(stream,))]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    print(" ".join(f"{kind}:{status}={value}" for (kind, status), value in sorted(statuses.items())))


if __name__ == "__main__":
    main()
