#!/usr/bin/env python3
"""WHEP playback lifecycle: the upstream media_server under test serves WHEP to pull sessions of a second server; control API without token."""
import argparse
import http.client
import json
import os
import secrets
import signal
import subprocess
import sys
import time
import uuid


def call(host, port, method, path, body=None):
    connection = http.client.HTTPConnection(host, port, timeout=10)
    started = time.monotonic()
    try:
        payload = json.dumps(body) if body is not None else None
        connection.request(method, path, payload, {"Content-Type": "application/json"} if payload else {})
        response = connection.getresponse()
        data = response.read()
        return response.status, data, (time.monotonic() - started) * 1000
    except OSError as error:
        return 0, str(error).encode(), (time.monotonic() - started) * 1000
    finally:
        connection.close()


class Server:
    def __init__(self, host, port, pid):
        self.host, self.port, self.pid = host, port, pid

    def receivers(self):
        status, data, _ = call(self.host, self.port, "GET", "/receivers")
        return {entry["stream_id"]: entry["ready"] for entry in json.loads(data)["receivers"]} if status == 200 else {}

    def create(self, stream, url):
        status, _, ms = call(self.host, self.port, "POST", "/pull/create", {"stream_id": stream, "url": url})
        return status, ms

    def delete(self, stream):
        status, _, ms = call(self.host, self.port, "POST", "/receivers/delete", {"stream_id": stream})
        return status, ms

    def udp_sockets(self):
        out = subprocess.run(["ss", "-uanp"], capture_output=True, text=True).stdout
        return sum(f"pid={self.pid}," in line for line in out.splitlines())

    def flv_first_tag_ms(self, stream):
        started = time.monotonic()
        connection = http.client.HTTPConnection(self.host, self.port, timeout=5)
        try:
            connection.request("GET", f"/{stream}/{secrets.token_hex(32)}.flv")
            response = connection.getresponse()
            if response.status != 200:
                return None
            response.read(9 + 4 + 11)
            return (time.monotonic() - started) * 1000
        except OSError:
            return None
        finally:
            connection.close()


def flv_status(host, port, stream):
    connection = http.client.HTTPConnection(host, port, timeout=5)
    try:
        connection.request("GET", f"/{stream}/{secrets.token_hex(32)}.flv")
        return connection.getresponse().status
    except OSError:
        return 0
    finally:
        connection.close()


def udp_count(pid):
    out = subprocess.run(["ss", "-uanp"], capture_output=True, text=True).stdout
    return sum(f"pid={pid}," in line for line in out.splitlines())


def wait(predicate, seconds, step=0.02):
    started = time.monotonic()
    while time.monotonic() - started < seconds:
        if predicate():
            return (time.monotonic() - started) * 1000
        time.sleep(step)
    return None


def percentile(values, fraction):
    values = sorted(v for v in values if v is not None)
    return round(values[min(len(values) - 1, int(fraction * len(values)))], 1) if values else None


def counts(values):
    result = {}
    for value in values:
        result[value] = result.get(value, 0) + 1
    return result


def report(name, **values):
    print(f"{name} " + " ".join(f"{key}={value}" for key, value in values.items()), flush=True)


def establish(server, url, n):
    streams = [str(uuid.uuid4()) for _ in range(n)]
    created = [server.create(stream, url) for stream in streams]
    started = time.monotonic()
    ready = {}
    while time.monotonic() - started < 20 and len(ready) < n:
        state = server.receivers()
        for stream in streams:
            if stream not in ready and state.get(stream):
                ready[stream] = (time.monotonic() - started) * 1000
        time.sleep(0.02)
    return streams, created, [ready.get(stream) for stream in streams]


def gone(server, streams):
    return lambda: not (set(streams) & set(server.receivers()))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True)
    parser.add_argument("--upstream", required=True)
    parser.add_argument("--server-pid", required=True)
    parser.add_argument("--upstream-pid", type=int, required=True)
    parser.add_argument("--publisher-pid", type=int, required=True)
    parser.add_argument("--stream", required=True)
    parser.add_argument("--restart-publisher", required=True)
    args = parser.parse_args()
    host, port = args.server.split(":")
    server = Server(host, int(port), args.server_pid)
    upstream_host, upstream_port = args.upstream.split(":")
    url = lambda stream: f"http://{upstream_host}:{upstream_port}/play/whep/{stream}/{secrets.token_hex(32)}"

    if wait(lambda: flv_status(upstream_host, int(upstream_port), args.stream) == 200, 30, 0.5) is None:
        report("upstream_ready", ok=False)
        return 1
    baseline = server.udp_sockets()
    upstream_baseline = udp_count(args.upstream_pid)
    upstream_back = lambda: wait(lambda: udp_count(args.upstream_pid) == upstream_baseline, 10)
    report("baseline", server_udp=baseline, upstream_udp=upstream_baseline)

    streams, created, ready = establish(server, url(args.stream), 8)
    first_tag = [server.flv_first_tag_ms(stream) for stream in streams]
    report("establish", create=counts(status for status, _ in created), create_ms_p50=percentile([ms for _, ms in created], 0.5),
           create_ms_max=percentile([ms for _, ms in created], 1), ready=sum(v is not None for v in ready), ready_ms_p50=percentile(ready, 0.5),
           ready_ms_max=percentile(ready, 1), flv_first_tag_ms_p50=percentile(first_tag, 0.5), flv_first_tag_ms_max=percentile(first_tag, 1),
           server_udp=server.udp_sockets(), upstream_udp=udp_count(args.upstream_pid))
    deleted = [server.delete(stream) for stream in streams]
    cleanup = wait(gone(server, streams), 10)
    sockets = wait(lambda: server.udp_sockets() == baseline, 10)
    report("delete", delete=counts(status for status, _ in deleted), delete_ms_max=percentile([ms for _, ms in deleted], 1),
           receivers_gone_ms=cleanup, udp_baseline_ms=sockets, upstream_udp_baseline_ms=upstream_back(), again=counts(server.delete(stream)[0] for stream in streams[:2]))

    statuses, ready_all, delete_statuses = [], [], []
    for _ in range(20):
        streams, created, ready = establish(server, url(args.stream), 8)
        statuses += [status for status, _ in created]
        ready_all += ready
        delete_statuses += [server.delete(stream)[0] for stream in streams]
    cleanup = wait(lambda: not server.receivers(), 10)
    sockets = wait(lambda: server.udp_sockets() == baseline, 10)
    report("churn", create=counts(statuses), ready=sum(v is not None for v in ready_all), creates=len(ready_all), ready_ms_p50=percentile(ready_all, 0.5),
           ready_ms_p99=percentile(ready_all, 0.99), delete=counts(delete_statuses), receivers_gone_ms=cleanup, udp_baseline_ms=sockets,
           server_udp=server.udp_sockets(), upstream_udp_baseline_ms=upstream_back())

    streams, created, ready = establish(server, url(args.stream), 4)
    dup = server.create(streams[0], url(args.stream))[0]
    invalid = server.create(str(uuid.uuid4()), "http:///nohost")[0]
    unreachable_stream = str(uuid.uuid4())
    unreachable = server.create(unreachable_stream, f"http://{upstream_host}:1/play/whep/x/y")[0]
    unreachable_gone = wait(gone(server, [unreachable_stream]), 30)
    missing_stream = str(uuid.uuid4())
    missing = server.create(missing_stream, url("0" * 64))[0]
    missing_gone = wait(gone(server, [missing_stream]), 30)
    report("invalid", ready=sum(v is not None for v in ready), duplicate=dup, bad_url=invalid, unreachable=unreachable, unreachable_gone_ms=unreachable_gone,
           missing_source=missing, missing_source_gone_ms=missing_gone)

    os.kill(args.publisher_pid, signal.SIGKILL)
    killed = time.monotonic()
    source_end = wait(gone(server, streams), 60, 0.1)
    report("source_end", receivers_gone_ms=source_end, server_udp=server.udp_sockets(), udp_baseline_ms=wait(lambda: server.udp_sockets() == baseline, 10),
           upstream_udp_baseline_ms=upstream_back())
    if source_end is None:
        report("source_end_cleanup", delete=counts(server.delete(stream)[0] for stream in streams), after_s=round(time.monotonic() - killed, 1))

    publisher = subprocess.Popen(args.restart_publisher, shell=True)
    if wait(lambda: flv_status(upstream_host, int(upstream_port), args.stream) == 200, 30, 0.5) is None:
        report("upstream_ready_again", ok=False)
        return 1
    streams, created, ready = establish(server, url(args.stream), 8)
    report("before_upstream_term", ready=sum(v is not None for v in ready), upstream_udp=udp_count(args.upstream_pid))
    return 0


if __name__ == "__main__":
    sys.exit(main())
