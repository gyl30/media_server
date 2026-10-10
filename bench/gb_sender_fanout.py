#!/usr/bin/env python3
"""Drive many GB28181 sender sessions from one asyncio process."""

import argparse
import asyncio
import ipaddress
import json
import socket
import time
import uuid
from dataclasses import dataclass
from pathlib import Path
from urllib.parse import urlsplit

from fanout_support import benchmark_head


@dataclass
class Sender:
    sender_id: str
    ssrc: int
    created_at: float


def percentile(values, fraction):
    if not values:
        return 0
    ordered = sorted(values)
    position = (len(ordered) - 1) * fraction
    lower = int(position)
    upper = min(lower + 1, len(ordered) - 1)
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)


def distribution(values):
    return {
        "min": min(values, default=0),
        "p10": percentile(values, 0.10),
        "p50": percentile(values, 0.50),
        "p90": percentile(values, 0.90),
        "max": max(values, default=0),
    }


class ControlClient:
    def __init__(self, base_url):
        parsed = urlsplit(base_url)
        if parsed.scheme != "http" or not parsed.hostname or parsed.query or parsed.fragment:
            raise ValueError("control URL must be an http URL without query or fragment")
        self.host = parsed.hostname
        self.port = parsed.port or 80
        self.base_path = parsed.path.rstrip("/")

    async def post(self, path, body):
        payload = json.dumps(body).encode()
        reader, writer = await asyncio.wait_for(
            asyncio.open_connection(self.host, self.port), timeout=5
        )
        try:
            target = self.base_path + path
            request = (
                f"POST {target} HTTP/1.1\r\n"
                f"Host: {self.host}:{self.port}\r\n"
                "Connection: close\r\n"
                "Content-Type: application/json\r\n"
                f"Content-Length: {len(payload)}\r\n\r\n"
            ).encode() + payload
            writer.write(request)
            await writer.drain()
            header = await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), timeout=10)
            lines = header.decode("iso-8859-1").split("\r\n")
            status = int(lines[0].split()[1])
            length = 0
            for line in lines[1:]:
                name, separator, value = line.partition(":")
                if separator and name.lower() == "content-length":
                    length = int(value.strip())
                    break
            response = (
                await asyncio.wait_for(reader.readexactly(length), timeout=10)
                if length
                else b""
            )
            return status, response.decode(errors="replace")
        finally:
            writer.close()
            await writer.wait_closed()


class State:
    def __init__(self, args):
        self.args = args
        self.expected = {}
        self.first_seen = set()
        self.first_media_ms = []
        self.bytes_by_ssrc = {}
        self.packets_by_ssrc = {}
        self.last_sequence = {}
        self.sequence_gaps = {}
        self.ready = asyncio.Event()
        self.measurement_started = False
        self.cleanup_started = False
        self.tcp_disconnects = 0
        self.errors = []

    def packet(self, packet, received_bytes):
        if (
            len(packet) < 12
            or packet[0] >> 6 != 2
            or packet[1] & 0x7F != self.args.payload_type
        ):
            return
        ssrc = int.from_bytes(packet[8:12], "big")
        sender = self.expected.get(ssrc)
        if sender is None:
            return

        sequence = int.from_bytes(packet[2:4], "big")
        previous = self.last_sequence.get(ssrc)
        if previous is not None:
            gap = (sequence - previous - 1) & 0xFFFF
            if gap < 0x8000:
                self.sequence_gaps[ssrc] = self.sequence_gaps.get(ssrc, 0) + gap
        self.last_sequence[ssrc] = sequence
        self.bytes_by_ssrc[ssrc] = self.bytes_by_ssrc.get(ssrc, 0) + received_bytes
        self.packets_by_ssrc[ssrc] = self.packets_by_ssrc.get(ssrc, 0) + 1
        if ssrc not in self.first_seen:
            self.first_seen.add(ssrc)
            self.first_media_ms.append((time.monotonic() - sender.created_at) * 1000)
            if len(self.first_seen) == self.args.viewers:
                self.ready.set()


class UdpSink(asyncio.DatagramProtocol):
    def __init__(self, state):
        self.state = state

    def datagram_received(self, data, _address):
        self.state.packet(data, len(data))


class TcpSink:
    def __init__(self, state):
        self.state = state
        self.writers = set()
        self.tasks = set()

    async def accepted(self, reader, writer):
        task = asyncio.current_task()
        self.tasks.add(task)
        self.writers.add(writer)
        try:
            while True:
                length = int.from_bytes(await reader.readexactly(2), "big")
                if length == 0:
                    raise RuntimeError("received a zero-length RFC4571 frame")
                packet = await reader.readexactly(length)
                self.state.packet(packet, length + 2)
        except asyncio.IncompleteReadError:
            if self.state.measurement_started and not self.state.cleanup_started:
                self.state.tcp_disconnects += 1
        except Exception as error:
            if not self.state.cleanup_started:
                self.state.errors.append(f"tcp_sink:{type(error).__name__}:{error}")
        finally:
            self.writers.discard(writer)
            self.tasks.discard(task)
            writer.close()
            try:
                await writer.wait_closed()
            except OSError:
                pass

    async def close(self):
        for writer in list(self.writers):
            writer.close()
        if self.writers:
            await asyncio.gather(
                *(writer.wait_closed() for writer in list(self.writers)),
                return_exceptions=True,
            )
        if self.tasks:
            await asyncio.gather(*list(self.tasks), return_exceptions=True)


async def create_sender(control, state, sender, port):
    body = {
        "stream_id": state.args.stream_name,
        "sender_id": sender.sender_id,
        "transport": state.args.transport,
        "payload_type": state.args.payload_type,
        "ssrc": sender.ssrc,
        "remote_address": state.args.host,
    }
    if state.args.transport == "udp":
        body.update(remote_rtp_port=port)
    else:
        body["remote_port"] = port
    status, response = await control.post("/gb28181/sender/create", body)
    if status != 201:
        raise RuntimeError(f"sender create returned HTTP {status}: {response}")


async def delete_sender(control, stream_name, sender):
    body = {
        "stream_id": stream_name,
        "sender_id": sender.sender_id,
    }
    status, response = await control.post("/gb28181/sender/delete", body)
    if status != 204:
        raise RuntimeError(f"sender delete returned HTTP {status}: {response}")


async def delete_senders(control, stream_name, senders):
    deleted = 0
    errors = []
    for offset in range(0, len(senders), 32):
        batch = senders[offset : offset + 32]
        results = await asyncio.gather(
            *(delete_sender(control, stream_name, sender) for sender in batch),
            return_exceptions=True,
        )
        for result in results:
            if isinstance(result, BaseException):
                errors.append(f"{type(result).__name__}:{result}")
            else:
                deleted += 1
    return deleted, errors


def make_udp_socket(host):
    address = ipaddress.ip_address(host)
    family = socket.AF_INET6 if address.version == 6 else socket.AF_INET
    sock = socket.socket(family, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 16 * 1024 * 1024)
    sock.bind((host, 0))
    sock.setblocking(False)
    return sock


def make_result(
    args,
    state,
    senders,
    establishment_seconds,
    elapsed,
    before_bytes,
    before_packets,
    before_sequence_gaps,
    deleted,
    cleanup_errors,
):
    if before_bytes is None or elapsed <= 0:
        byte_deltas = [0] * len(senders)
        packet_deltas = [0] * len(senders)
        sequence_gap_deltas = [0] * len(senders)
    else:
        byte_deltas = [
            state.bytes_by_ssrc.get(sender.ssrc, 0) - before_bytes.get(sender.ssrc, 0)
            for sender in senders
        ]
        packet_deltas = [
            state.packets_by_ssrc.get(sender.ssrc, 0) - before_packets.get(sender.ssrc, 0)
            for sender in senders
        ]
        sequence_gap_deltas = [
            state.sequence_gaps.get(sender.ssrc, 0)
            - before_sequence_gaps.get(sender.ssrc, 0)
            for sender in senders
        ]
    byte_rates = [value / elapsed for value in byte_deltas] if elapsed else []
    packet_rates = [value / elapsed for value in packet_deltas] if elapsed else []
    progressing = sum(
        byte_count > 0 and packet_count > 0
        for byte_count, packet_count in zip(byte_deltas, packet_deltas)
    )
    return {
        "config": {
            "head": benchmark_head(),
            "control_url": args.control_url,
            "stream_name": args.stream_name,
            "transport": args.transport,
            "host": args.host,
            "viewers": args.viewers,
            "payload_type": args.payload_type,
            "ramp_per_second": args.ramp_per_second,
            "warmup_seconds": args.warmup,
            "duration_seconds": args.duration,
        },
        "establishment": {
            "created": len(senders),
            "ready": len(state.first_seen),
            "seconds": establishment_seconds,
            "first_media_milliseconds": distribution(state.first_media_ms),
        },
        "measurement": {
            "duration_seconds": elapsed,
            "progressing": progressing,
            "received_bytes": sum(byte_deltas),
            "received_packets": sum(packet_deltas),
            "aggregate_gbit_per_second": 8 * sum(byte_deltas) / elapsed / 1e9 if elapsed else 0,
            "packets_per_second": sum(packet_deltas) / elapsed if elapsed else 0,
            "viewer_bytes_per_second": distribution(byte_rates),
            "viewer_packets_per_second": distribution(packet_rates),
            "sequence_gaps": sum(sequence_gap_deltas),
            "tcp_disconnects": state.tcp_disconnects,
        },
        "cleanup": {
            "deleted": deleted,
            "errors": cleanup_errors,
        },
        "errors": state.errors,
    }


async def run(args):
    control = ControlClient(args.control_url)
    state = State(args)
    senders = []
    attempted_senders = []
    udp_transports = []
    tcp_server = None
    tcp_sink = None
    deleted = 0
    cleanup_errors = []
    establishment_started = time.monotonic()
    establishment_seconds = 0
    elapsed = 0
    before_bytes = None
    before_packets = None
    before_sequence_gaps = None

    try:
        loop = asyncio.get_running_loop()
        shared_tcp_port = None
        if args.transport == "tcp_active":
            tcp_sink = TcpSink(state)
            tcp_server = await asyncio.start_server(
                tcp_sink.accepted,
                args.host,
                args.listen_port,
                backlog=args.viewers + 32,
            )
            shared_tcp_port = tcp_server.sockets[0].getsockname()[1]

        initial_ssrc = (uuid.uuid4().int & 0x7FFFF000) | 1
        next_create = time.monotonic()
        for index in range(args.viewers):
            ssrc = (initial_ssrc + index) & 0xFFFFFFFF or index + 1
            sender = Sender(str(uuid.uuid4()), ssrc, time.monotonic())
            attempted_senders.append(sender)
            state.expected[ssrc] = sender
            transport = None
            try:
                if args.transport == "udp":
                    sock = make_udp_socket(args.host)
                    transport, _ = await loop.create_datagram_endpoint(
                        lambda: UdpSink(state), sock=sock
                    )
                    port = transport.get_extra_info("sockname")[1]
                else:
                    port = shared_tcp_port
                await create_sender(control, state, sender, port)
            except Exception:
                state.expected.pop(ssrc, None)
                if transport is not None:
                    transport.close()
                raise
            senders.append(sender)
            if transport is not None:
                udp_transports.append(transport)
            if index + 1 < args.viewers:
                next_create += 1 / args.ramp_per_second
                await asyncio.sleep(max(0, next_create - time.monotonic()))

        try:
            await asyncio.wait_for(
                state.ready.wait(),
                timeout=max(30, args.viewers / args.ramp_per_second + 20),
            )
        except asyncio.TimeoutError as error:
            raise RuntimeError(
                f"only {len(state.first_seen)} of {args.viewers} senders produced RTP"
            ) from error

        establishment_seconds = time.monotonic() - establishment_started
        print(
            f"phase=established created={len(senders)} ready={len(state.first_seen)} "
            f"seconds={establishment_seconds:.6f}",
            flush=True,
        )
        await asyncio.sleep(args.warmup)
        before_bytes = state.bytes_by_ssrc.copy()
        before_packets = state.packets_by_ssrc.copy()
        before_sequence_gaps = state.sequence_gaps.copy()
        state.measurement_started = True
        print("phase=measurement_start", flush=True)
        started = time.monotonic()
        await asyncio.sleep(args.duration)
        elapsed = time.monotonic() - started
    except Exception as error:
        state.errors.append(f"{type(error).__name__}:{error}")
    finally:
        state.cleanup_started = True
        deleted, cleanup_errors = await delete_senders(
            control, args.stream_name, attempted_senders
        )
        if tcp_server is not None:
            tcp_server.close()
            await tcp_server.wait_closed()
        if tcp_sink is not None:
            await tcp_sink.close()
        for transport in udp_transports:
            transport.close()

    result = make_result(
        args,
        state,
        senders,
        establishment_seconds,
        elapsed,
        before_bytes,
        before_packets,
        before_sequence_gaps,
        deleted,
        cleanup_errors,
    )
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))
    measurement = result["measurement"]
    success = (
        len(senders) == args.viewers
        and len(state.first_seen) == args.viewers
        and measurement["progressing"] == args.viewers
        and not measurement["tcp_disconnects"]
        and deleted == len(attempted_senders)
        and not cleanup_errors
        and not state.errors
    )
    return 0 if success else 1


def parse_arguments():
    parser = argparse.ArgumentParser(
        description="Fan out one existing media stream through many GB28181 sender sessions"
    )
    parser.add_argument("--control-url", required=True, help="media server HTTP control base URL")
    parser.add_argument("--stream-name", required=True, help="existing source stream name")
    parser.add_argument(
        "--transport", choices=("udp", "tcp_active"), required=True
    )
    parser.add_argument(
        "--host",
        default="127.0.0.1",
        help="local numeric address to bind and advertise as the RTP sink",
    )
    parser.add_argument("--viewers", type=int, required=True)
    parser.add_argument("--payload-type", type=int, default=96)
    parser.add_argument("--ramp-per-second", type=int, default=100)
    parser.add_argument("--listen-port", type=int, default=0, help="TCP sink port; zero selects an ephemeral port")
    parser.add_argument("--warmup", type=float, default=3)
    parser.add_argument("--duration", type=float, default=15)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        ipaddress.ip_address(args.host)
        ControlClient(args.control_url)
    except ValueError as error:
        parser.error(str(error))
    if (
        args.viewers < 1
        or args.ramp_per_second < 1
        or args.duration <= 0
        or args.warmup < 0
        or not 0 <= args.payload_type <= 127
        or not 0 <= args.listen_port <= 65535
    ):
        parser.error(
            "viewers, ramp and duration must be positive; warmup, payload type and listen port must be valid"
        )
    return args


def main():
    args = parse_arguments()
    try:
        return asyncio.run(run(args))
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
