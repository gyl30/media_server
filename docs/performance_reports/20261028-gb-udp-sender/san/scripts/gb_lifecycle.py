#!/usr/bin/env python3
"""GB UDP sender lifecycle driver for one media_server (loopback control API without token)."""
import argparse
import json
import os
import random
import select
import socket
import struct
import subprocess
import sys
import time
import urllib.error
import urllib.request
import uuid

PEER_ADDRESS = "127.0.0.2"


def post(base, path, body):
    request = urllib.request.Request(base + path, json.dumps(body).encode(), {"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(request, timeout=10) as response:
            return response.status
    except urllib.error.HTTPError as error:
        return error.code


def server_sockets(pid):
    out = subprocess.run(["ss", "-uanmp"], capture_output=True, text=True).stdout.splitlines()
    sockets = []
    for index, line in enumerate(out):
        if f"pid={pid}," not in line:
            continue
        fields = line.split()
        port = int(fields[3].rsplit(":", 1)[1])
        if port < 49152:
            continue
        mem = out[index + 1] if index + 1 < len(out) else ""
        sockets.append((port, int(fields[1]), mem.strip()))
    return sockets


def rtcp_backlog(pid):
    return sum(recvq for port, recvq, _ in server_sockets(pid) if port % 2)


class Peer:
    def __init__(self, stream, rtcp, rr):
        self.stream = stream
        self.sender_id = str(uuid.uuid4())
        self.ssrc = random.randrange(1, 1 << 31)
        self.rtcp = rtcp
        self.rr = rr
        self.rtp_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.rtp_socket.bind((PEER_ADDRESS, 0))
        self.rtp_socket.setblocking(False)
        self.rtcp_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.rtcp_socket.bind((PEER_ADDRESS, 0))
        self.rtcp_socket.setblocking(False)
        self.packets = 0
        self.bad = 0
        self.gaps = 0
        self.last_sequence = None
        self.source = None
        self.sender_reports = 0
        self.receiver_reports = 0
        self.closed = False

    def body(self):
        body = {"stream_id": self.stream, "sender_id": self.sender_id, "transport": "udp", "remote_address": PEER_ADDRESS,
                "remote_rtp_port": self.rtp_socket.getsockname()[1], "payload_type": 96, "ssrc": self.ssrc}
        if self.rtcp:
            body["remote_rtcp_port"] = self.rtcp_socket.getsockname()[1]
        return body

    def sockets(self):
        return [] if self.closed else [self.rtp_socket, self.rtcp_socket]

    def readable(self, sock):
        while True:
            try:
                data, source = sock.recvfrom(65536)
            except BlockingIOError:
                return
            if sock is self.rtcp_socket:
                if len(data) >= 28 and data[0] >> 6 == 2 and data[1] == 200 and struct.unpack("!I", data[4:8])[0] == self.ssrc:
                    self.sender_reports += 1
                else:
                    self.bad += 1
                continue
            self.source = source
            if len(data) < 12 or data[0] >> 6 != 2 or data[1] & 0x7F != 96 or struct.unpack("!I", data[8:12])[0] != self.ssrc:
                self.bad += 1
                continue
            sequence = struct.unpack("!H", data[2:4])[0]
            if self.last_sequence is not None and (sequence - self.last_sequence) & 0xFFFF != 1:
                self.gaps += 1
            self.last_sequence = sequence
            self.packets += 1

    def feedback(self):
        if self.closed or not self.rr or self.source is None:
            return
        report = struct.pack("!BBHII", 0x81, 201, 7, self.ssrc ^ 0x80000000, self.ssrc) + struct.pack("!IIIII", 0, self.last_sequence or 0, 0, 0, 0)
        report += struct.pack("!BBHI", 0x81, 202, 4, self.ssrc ^ 0x80000000) + b"\x01\x08gbclient\x00\x00"
        self.rtcp_socket.sendto(report, (self.source[0], self.source[1] + 1))
        self.receiver_reports += 1

    def close(self):
        self.closed = True
        self.rtp_socket.close()
        self.rtcp_socket.close()


def pump(peers, seconds):
    deadline = time.monotonic() + seconds
    next_feedback = time.monotonic() + 1
    while True:
        now = time.monotonic()
        if now >= deadline:
            return
        sockets = {sock: peer for peer in peers for sock in peer.sockets()}
        readable, _, _ = select.select(list(sockets), [], [], min(deadline, next_feedback) - now) if sockets else ([], [], time.sleep(max(0, min(deadline, next_feedback) - now)))
        for sock in readable:
            sockets[sock].readable(sock)
        if time.monotonic() >= next_feedback:
            for peer in peers:
                peer.feedback()
            next_feedback += 1


def create(base, peers):
    return [post(base, "/gb28181/sender/create", peer.body()) for peer in peers]


def delete(base, peers):
    return [post(base, "/gb28181/sender/delete", {"stream_id": peer.stream, "sender_id": peer.sender_id}) for peer in peers]


def counts(values):
    result = {}
    for value in values:
        result[value] = result.get(value, 0) + 1
    return result


def report(name, **values):
    print(f"{name} " + " ".join(f"{key}={value}" for key, value in values.items()), flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--http", required=True)
    parser.add_argument("--stream", required=True)
    parser.add_argument("--server-pid", required=True)
    parser.add_argument("--publisher-pid", type=int, required=True)
    parser.add_argument("--phase", choices=("main", "hold"), default="main")
    args = parser.parse_args()
    base = args.http
    pid = args.server_pid

    deadline = time.monotonic() + 30
    while True:
        probe = Peer(args.stream, False, False)
        if create(base, [probe]) == [201]:
            delete(base, [probe])
            probe.close()
            break
        probe.close()
        if time.monotonic() > deadline:
            report("source_ready", ok=False)
            return 1
        time.sleep(0.5)

    if args.phase == "hold":
        peers = [Peer(args.stream, index % 2 == 0, True) for index in range(8)]
        report("hold_create", statuses=counts(create(base, peers)))
        while True:
            pump(peers, 1)

    report("baseline", server_sockets=len(server_sockets(pid)))

    created, deleted, ports, starved = [], [], [], 0
    for round_index in range(30):
        peers = [Peer(args.stream, index % 2 == 0, True) for index in range(8)]
        created += create(base, peers)
        pump(peers, 3)
        starved += sum(peer.packets == 0 for peer in peers)
        ports += [peer.source[1] for peer in peers if peer.source]
        deleted += delete(base, peers)
        for peer in peers:
            peer.close()
    report("churn", create=counts(created), delete=counts(deleted), starved=starved, unique_ports=len(set(ports)), creates=len(created),
           server_sockets_after=len(server_sockets(pid)))

    peers = [Peer(args.stream, True, True) for _ in range(4)]
    statuses = create(base, peers)
    pump(peers, 28)
    report("rtcp_sr", create=counts(statuses), sender_reports=[peer.sender_reports for peer in peers],
           receiver_reports=sum(peer.receiver_reports for peer in peers), gaps=sum(peer.gaps for peer in peers),
           bad=sum(peer.bad for peer in peers), server_rtcp_recvq=rtcp_backlog(pid))
    report("rtcp_sr_delete", delete=counts(delete(base, peers)), server_sockets_after=len(server_sockets(pid)))
    for peer in peers:
        peer.close()

    peers = [Peer(args.stream, index % 2 == 0, True) for index in range(4)]
    statuses = create(base, peers)
    pump(peers, 3)
    for peer in peers:
        peer.close()
    time.sleep(10)
    sockets = server_sockets(pid)
    report("peer_gone", create=counts(statuses), server_sockets=len(sockets), send_queue=sum("t0," not in mem for _, _, mem in sockets),
           delete=counts(delete(base, peers)), server_sockets_after=len(server_sockets(pid)))

    peers = [Peer(args.stream, True, False) for _ in range(4)]
    statuses = create(base, peers)
    pump(peers, 2)
    before = [peer.packets for peer in peers]
    garbage = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sent = 0
    for peer in peers:
        for port in (peer.source[1], peer.source[1] + 1):
            for size in (1, 12, 13, 64, 200, 1400, 1501, 9000, 65000):
                for first in (0x00, 0x80, 0x81, 0xC8, 0xC9, 0xFF):
                    garbage.sendto(bytes([first]) + os.urandom(size - 1), ("127.0.0.1", port))
                    sent += 1
    garbage.close()
    pump(peers, 3)
    report("garbage", create=counts(statuses), datagrams=sent, progressing=sum(peer.packets > count for peer, count in zip(peers, before)),
           gaps=sum(peer.gaps for peer in peers), server_rtcp_recvq=rtcp_backlog(pid), delete=counts(delete(base, peers)))
    for peer in peers:
        peer.close()

    probe = Peer(args.stream, False, False)
    invalid = probe.body()
    invalid["remote_rtcp_port"] = invalid["remote_rtp_port"]
    missing = Peer("missing-" + uuid.uuid4().hex, False, False)
    duplicate = Peer(args.stream, False, False)
    duplicate_statuses = create(base, [duplicate, duplicate])
    report("invalid", same_ports=post(base, "/gb28181/sender/create", invalid), missing_stream=create(base, [missing])[0],
           duplicate=duplicate_statuses, unknown_delete=post(base, "/gb28181/sender/delete", {"stream_id": args.stream, "sender_id": "unknown"}),
           duplicate_delete=delete(base, [duplicate]))
    for peer in (probe, missing, duplicate):
        peer.close()

    peers = [Peer(args.stream, index % 2 == 0, True) for index in range(8)]
    statuses = create(base, peers)
    pump(peers, 3)
    os.kill(args.publisher_pid, 9)
    pump(peers, 2)
    before = [peer.packets for peer in peers]
    pump(peers, 3)
    report("source_end", create=counts(statuses), still_sending=sum(peer.packets > count for peer, count in zip(peers, before)),
           server_sockets_after=len(server_sockets(pid)), delete=counts(delete(base, peers)))
    for peer in peers:
        peer.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
