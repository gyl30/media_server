#!/usr/bin/env python3
"""Drive gb_fanout against a fake media_server: answer /gb28181/sender/create and /delete, then send each viewer RTP/PS replayed from a
real GB dump (rewritten to the viewer's SSRC) over UDP, TCP active or TCP passive. In a malformed run every viewer gets valid packets followed
by the --fault packet; in an abort run the sender vanishes mid-stream. Meant to run gb_fanout under ASan or TSan; it reports
gb_fanout's exit status and the sanitizer log files, not media quality."""

import argparse
import json
import os
import socket
import struct
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

VALID_BEFORE_FAULT = 400
PACKETS_PER_SECOND = 500


def read_records(path):
    data = path.read_bytes()
    records = []
    offset = 0
    while offset + 2 <= len(data):
        length = struct.unpack_from("!H", data, offset)[0]
        records.append(bytes(data[offset + 2:offset + 2 + length]))
        offset += 2 + length
    return records


def header(sequence, timestamp, ssrc, payload_type, first=0x80):
    return struct.pack("!BBHII", first, payload_type, sequence & 0xFFFF, timestamp, ssrc)


def malformed(sequence, timestamp, ssrc, payload_type):
    good = header(sequence, timestamp, ssrc, payload_type)
    return [
        ("one_byte", b"\x80"),
        ("eleven_bytes", good[:11]),
        ("version_1", header(sequence, timestamp, ssrc, payload_type, 0x40) + b"\x00\x00\x01\xba"),
        ("csrc_beyond_packet", header(sequence, timestamp, ssrc, payload_type, 0x8F) + b"\x00" * 8),
        ("extension_beyond_packet", header(sequence, timestamp, ssrc, payload_type, 0x90) + b"\xbe\xde\xff\xff"),
        ("padding_flag", header(sequence, timestamp, ssrc, payload_type, 0xA0) + b"\x00\x00\x01\xba" + b"\x00" * 10 + b"\x10"),
        ("wrong_payload_type", header(sequence, timestamp, ssrc, (payload_type + 1) & 0x7F) + b"\x00\x00\x01\xba"),
        ("wrong_ssrc", header(sequence, timestamp, ssrc ^ 1, payload_type) + b"\x00\x00\x01\xba"),
        ("sequence_gap", header(sequence + 7, timestamp, ssrc, payload_type) + b"\x00\x00\x01\xba"),
        ("continuation_without_pack", header(sequence, timestamp + 3000, ssrc, payload_type) + b"\x12" * 300),
        ("pack_header_truncated", good + b"\x00\x00\x01\xba\x44\x00\x04"),
        ("pes_pts_truncated", good + b"\x00\x00\x01\xba\x44\x00\x04\x00\x04\x01\x01\x89\xc3\xf8" + b"\x00\x00\x01\xc0\x00\x08\x80\x80\x05\x21"),
        ("payload_empty", good),
        ("oversized", good + b"\x00\x00\x01\xba" + b"\x55" * 60000),
        ("pes_length_beyond_packet", good + b"\x00\x00\x01\xba\x44\x00\x04\x00\x04\x01\x01\x89\xc3\xf8" + b"\x00\x00\x01\xe0\xff\xff\x80\x80\x05"),
    ]


class Session:
    def __init__(self, args, request, index):
        self.args = args
        self.request = request
        self.index = index
        self.ssrc = int(request["ssrc"])
        self.payload_type = int(request["payload_type"])
        self.fault = None
        self.sent = 0
        self.listener = None
        if request["transport"] == "tcp_passive":
            self.listener = socket.create_server((args.bind_address, int(request["listen_port"])))
            self.listener.settimeout(10)

    def packets(self):
        records = self.args.records
        first_sequence = struct.unpack_from("!H", records[0], 2)[0]
        for position, record in enumerate(records):
            rewritten = bytearray(record)
            struct.pack_into("!I", rewritten, 8, self.ssrc)
            struct.pack_into("!B", rewritten, 1, (record[1] & 0x80) | self.payload_type)
            struct.pack_into("!H", rewritten, 2, (first_sequence + position) & 0xFFFF)
            if self.args.mode != "valid" and position == VALID_BEFORE_FAULT:
                if self.args.mode == "abort":
                    self.fault = "abort"
                    return
                timestamp = struct.unpack_from("!I", record, 4)[0]
                self.fault = self.args.fault
                yield dict(malformed(first_sequence + position, timestamp, self.ssrc, self.payload_type))[self.args.fault]
                return
            yield bytes(rewritten)

    def run(self):
        transport = self.request["transport"]
        if transport == "udp":
            target = (self.request["remote_address"], int(self.request["remote_rtp_port"]))
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
                for packet in self.packets():
                    if len(packet) <= 65507:
                        sock.sendto(packet, target)
                    self.pace()
            return
        if transport == "tcp_active":
            sock = socket.create_connection((self.request["remote_address"], int(self.request["remote_port"])), timeout=10)
        else:
            with self.listener:
                sock, _ = self.listener.accept()
        with sock:
            try:
                for packet in self.packets():
                    sock.sendall(struct.pack("!H", len(packet)) + packet)
                    self.pace()
                if self.fault == "abort":
                    sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
            except OSError:
                pass

    def pace(self):
        self.sent += 1
        time.sleep(1 / PACKETS_PER_SECOND)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gb-fanout", type=Path, required=True)
    parser.add_argument("--dump", type=Path, required=True, help="RTP/PS dump from gb_fanout --dump-file")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--transport", choices=("udp", "tcp_active", "tcp_passive"), required=True)
    parser.add_argument("--mode", choices=("valid", "malformed", "abort"), required=True)
    parser.add_argument("--fault", choices=[name for name, _ in malformed(0, 0, 0, 0)], help="malformed packet sent in a malformed run")
    parser.add_argument("--viewers", type=int, default=3)
    parser.add_argument("--duration", type=int, default=6)
    parser.add_argument("--dump-file", default="", help="passed to gb_fanout --dump-file")
    parser.add_argument("--bind-address", default="127.0.0.1")
    parser.add_argument("--listen-port", type=int, default=46000)
    parser.add_argument("--env", action="append", default=[], help="KEY=VALUE for gb_fanout, e.g. ASAN_OPTIONS=...")
    args = parser.parse_args()
    if (args.mode == "malformed") != (args.fault is not None):
        parser.error("--fault is required in malformed mode only")
    args.output.mkdir(parents=True)
    args.records = read_records(args.dump)
    sessions = []
    lock = threading.Lock()

    class Control(BaseHTTPRequestHandler):
        def do_POST(self):
            body = json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))) or b"{}")
            if self.path == "/gb28181/sender/create":
                with lock:
                    session = Session(args, body, len(sessions))
                    sessions.append(session)
                self.send_response(201)
                self.send_header("Content-Length", "0")
                self.end_headers()
                threading.Thread(target=lambda: (time.sleep(0.2), session.run()), daemon=True).start()
                return
            self.send_response(204 if self.path == "/gb28181/sender/delete" else 404)
            self.send_header("Content-Length", "0")
            self.end_headers()

        def log_message(self, *_):
            pass

    control = ThreadingHTTPServer((args.bind_address, 0), Control)
    threading.Thread(target=control.serve_forever, daemon=True).start()
    (args.output / "token").write_text("harness\n")
    (args.output / "paths").write_text("".join(f"stream{index}/sender{index}\n" for index in range(args.viewers)))
    command = [str(args.gb_fanout), "--control-token-file", str(args.output / "token"), "--transport", args.transport, "--bind-address",
               args.bind_address, "--listen-port", str(args.listen_port), "--play-paths-file", str(args.output / "paths"), "--media-host",
               args.bind_address, "--media-port", str(control.server_port), "--viewers", str(args.viewers), "--duration", str(args.duration),
               "--warmup", "1", "--ramp-per-second", "50", "--ready-timeout", "8"]
    command += ["--dump-file", args.dump_file] if args.dump_file else []
    environment = {**os.environ, **dict(item.split("=", 1) for item in args.env)}
    with (args.output / "gb_fanout.log").open("w") as log:
        process = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, env=environment, timeout=args.duration + 60)
    control.shutdown()
    reports = sorted(str(path) for path in args.output.glob("sanitizer*"))
    findings = [path for path in reports if any(marker in Path(path).read_text(errors="replace") for marker in ("ERROR:", "WARNING: ThreadSanitizer"))]
    result = {"transport": args.transport, "mode": args.mode, "exit": process.returncode, "signaled": process.returncode < 0,
              "faults": sorted({session.fault for session in sessions if session.fault}), "sessions": len(sessions),
              "sanitizer_logs": reports, "sanitizer_findings": findings}
    (args.output / "result.json").write_text(json.dumps(result, indent=1))
    print(json.dumps(result))


if __name__ == "__main__":
    main()
