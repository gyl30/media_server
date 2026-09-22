#!/usr/bin/env python3
"""Sharded HLS capacity client with protocol-level progress checks and drained media bodies."""

import argparse
import asyncio
import json
import statistics
import time
from dataclasses import dataclass, field
from typing import Optional
from urllib.parse import urljoin, urlsplit


def unix_now_ns() -> int:
    return time.time_ns()


def error_text(error: BaseException) -> str:
    return f"{type(error).__name__}:{error}"


async def read_body(reader: asyncio.StreamReader, length: int, retain: bool) -> bytes:
    chunks: list[bytes] = []
    remaining = length
    while remaining:
        chunk = await reader.readexactly(min(remaining, 64 * 1024))
        if retain:
            chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


async def http_request(method: str, url: str, body: bytes = b"", retain_body: bool = True) -> tuple[int, dict[str, str], bytes]:
    parsed = urlsplit(url)
    if parsed.scheme != "http" or not parsed.hostname:
        raise ValueError("URL must use http")
    host = parsed.hostname
    port = parsed.port or 80
    target = parsed.path or "/"
    if parsed.query:
        target += "?" + parsed.query
    reader, writer = await asyncio.wait_for(asyncio.open_connection(host, port), timeout=5)
    try:
        headers = [
            f"{method} {target} HTTP/1.1",
            f"Host: {host}",
            "Connection: close",
            "User-Agent: media_server_fanout_hls",
        ]
        if body:
            headers.extend(("Content-Type: application/json", f"Content-Length: {len(body)}"))
        writer.write(("\r\n".join(headers) + "\r\n\r\n").encode() + body)
        await writer.drain()
        head = await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), timeout=10)
        lines = head.decode("iso-8859-1").split("\r\n")
        status = int(lines[0].split()[1])
        response_headers: dict[str, str] = {}
        for line in lines[1:]:
            if not line:
                continue
            key, value = line.split(":", 1)
            response_headers[key.lower()] = value.strip()
        length = int(response_headers.get("content-length", "0"))
        response = await asyncio.wait_for(read_body(reader, length, retain_body), timeout=10) if length else b""
        return status, response_headers, response
    finally:
        writer.close()
        await writer.wait_closed()


class HttpClient:
    def __init__(self, state: "State") -> None:
        self._state = state
        self._reader: Optional[asyncio.StreamReader] = None
        self._writer: Optional[asyncio.StreamWriter] = None
        self._endpoint: Optional[tuple[str, int]] = None

    async def request(self, method: str, url: str, body: bytes = b"", retain_body: bool = True) -> tuple[int, dict[str, str], bytes]:
        if not self._state.args.persistent:
            self._state.result.media_connections += 1
            return await http_request(method, url, body, retain_body)

        parsed = urlsplit(url)
        if parsed.scheme != "http" or not parsed.hostname:
            raise ValueError("URL must use http")
        host = parsed.hostname
        port = parsed.port or 80
        target = parsed.path or "/"
        if parsed.query:
            target += "?" + parsed.query
        if self._endpoint != (host, port):
            await self.close()
        if self._writer is None:
            self._reader, self._writer = await asyncio.wait_for(asyncio.open_connection(host, port), timeout=5)
            self._endpoint = (host, port)
            self._state.result.media_connections += 1

        headers = [
            f"{method} {target} HTTP/1.1",
            f"Host: {host}",
            "Connection: keep-alive",
            "User-Agent: media_server_fanout_hls",
        ]
        if body:
            headers.extend(("Content-Type: application/json", f"Content-Length: {len(body)}"))
        self._writer.write(("\r\n".join(headers) + "\r\n\r\n").encode() + body)
        await self._writer.drain()
        head = await asyncio.wait_for(self._reader.readuntil(b"\r\n\r\n"), timeout=10)
        lines = head.decode("iso-8859-1").split("\r\n")
        status = int(lines[0].split()[1])
        response_headers: dict[str, str] = {}
        for line in lines[1:]:
            if not line:
                continue
            key, value = line.split(":", 1)
            response_headers[key.lower()] = value.strip()
        length = int(response_headers.get("content-length", "0"))
        response = await asyncio.wait_for(read_body(self._reader, length, retain_body), timeout=10) if length else b""
        if response_headers.get("connection", "").lower() == "close":
            await self.close()
        return status, response_headers, response

    async def close(self) -> None:
        if self._writer is None:
            return
        self._writer.close()
        try:
            await self._writer.wait_closed()
        except OSError:
            pass
        self._reader = None
        self._writer = None
        self._endpoint = None


@dataclass
class Results:
    allocated: int = 0
    connected: int = 0
    ready: int = 0
    progressing: int = 0
    completed: int = 0
    failures: int = 0
    pre_measurement_failures: int = 0
    trigger_failures: int = 0
    pre_measurement_abort_viewers: int = 0
    collateral_aborted_viewers: int = 0
    runtime_failures: int = 0
    non_progressing_viewers: int = 0
    failure_phases: dict[str, int] = field(default_factory=dict)
    runtime_errors: dict[str, int] = field(default_factory=dict)
    first_media_milliseconds: list[int] = field(default_factory=list)
    received_video_bytes: int = 0
    received_video_messages: int = 0
    steady_video_bytes: int = 0
    steady_video_messages: int = 0
    measurement_start_unix_ns: int = 0
    measurement_end_unix_ns: int = 0
    trigger_unix_ns: int = 0
    trigger_failure_phase: str = ""
    trigger_error: str = ""
    media_connections: int = 0


class State:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.result = Results()
        self.lock = asyncio.Lock()
        self.ready_event = asyncio.Event()
        self.abort_event = asyncio.Event()
        self.measurement_started = False
        self.measurement_deadline = 0.0
        self.trigger_viewer = -1

    async def fail_before_measurement(self, viewer: int, phase: str, error: BaseException) -> None:
        async with self.lock:
            self.result.failures += 1
            self.result.failure_phases[phase] = self.result.failure_phases.get(phase, 0) + 1
            if not self.measurement_started and not self.abort_event.is_set():
                self.result.pre_measurement_failures += 1
                self.result.trigger_failures += 1
                self.result.trigger_failure_phase = phase
                self.result.trigger_error = error_text(error)
                self.result.trigger_unix_ns = unix_now_ns()
                self.trigger_viewer = viewer
                self.abort_event.set()
                self.ready_event.set()

    async def mark_ready(self, first_media_ms: int) -> None:
        async with self.lock:
            self.result.connected += 1
            self.result.ready += 1
            self.result.first_media_milliseconds.append(first_media_ms)
            if self.result.ready == self.args.viewers and not self.abort_event.is_set():
                self.measurement_started = True
                self.measurement_deadline = time.monotonic() + self.args.duration
                self.result.measurement_start_unix_ns = unix_now_ns()
                self.result.measurement_end_unix_ns = self.result.measurement_start_unix_ns + self.args.duration * 1_000_000_000
                self.ready_event.set()


async def allocate(state: State) -> str:
    request = json.dumps({"protocol": "hls", "stream_name": state.args.stream_name}).encode()
    status, _, response = await http_request("POST", state.args.signaling_url + "/api/play/allocations", request)
    if status != 201:
        raise RuntimeError(f"allocation status={status}")
    value = json.loads(response)
    return value["play_url"]


def playlist_paths(playlist_url: str, text: str) -> tuple[Optional[str], Optional[str], int]:
    init_url: Optional[str] = None
    segment_url: Optional[str] = None
    target_duration = 2
    for line in text.splitlines():
        if line.startswith("#EXT-X-TARGETDURATION:"):
            target_duration = max(1, int(line.split(":", 1)[1]))
        elif line.startswith("#EXT-X-MAP:"):
            marker = 'URI="'
            begin = line.find(marker)
            if begin >= 0:
                end = line.find('"', begin + len(marker))
                if end >= 0:
                    init_url = urljoin(playlist_url, line[begin + len(marker) : end])
        elif line and not line.startswith("#"):
            segment_url = urljoin(playlist_url, line)
    return init_url, segment_url, target_duration


async def request_playlist(client: HttpClient, url: str) -> tuple[Optional[str], Optional[str], int, int]:
    status, _, response = await client.request("GET", url)
    if status != 200:
        raise RuntimeError(f"playlist status={status}")
    text = response.decode("utf-8")
    if "#EXTM3U" not in text:
        raise RuntimeError("invalid playlist")
    init_url, segment_url, target_duration = playlist_paths(url, text)
    if not segment_url:
        raise RuntimeError("playlist has no segment")
    return init_url, segment_url, target_duration, len(response)


async def request_media(client: HttpClient, url: str) -> int:
    status, headers, _ = await client.request("GET", url, retain_body=False)
    bytes_received = int(headers.get("content-length", "0"))
    if status != 200 or bytes_received <= 0:
        raise RuntimeError(f"media status={status} bytes={bytes_received}")
    return bytes_received


async def run_viewer(state: State, index: int) -> None:
    started = time.monotonic()
    bytes_received = 0
    messages_received = 0
    steady_bytes_before = 0
    steady_messages_before = 0
    initialized = False
    last_segment: Optional[str] = None
    poll_seconds = 2
    client = HttpClient(state)
    try:
        play_url = await allocate(state)
        state.result.allocated += 1
        status, headers, _ = await client.request("GET", play_url)
        if status != 307 or "location" not in headers:
            raise RuntimeError(f"redirect status={status}")
        playlist_url = urljoin(play_url, headers["location"])
        init_url, segment_url, poll_seconds, playlist_bytes = await request_playlist(client, playlist_url)
        bytes_received += playlist_bytes
        if init_url:
            bytes_received += await request_media(client, init_url)
        bytes_received += await request_media(client, segment_url)
        messages_received += 1
        last_segment = segment_url
        initialized = True
        await state.mark_ready(int((time.monotonic() - started) * 1000))

        await state.ready_event.wait()
        if state.abort_event.is_set():
            return
        steady_bytes_before = bytes_received
        steady_messages_before = messages_received

        while not state.abort_event.is_set():
            if state.measurement_started and time.monotonic() >= state.measurement_deadline:
                break
            await asyncio.sleep(poll_seconds)
            init_url, segment_url, poll_seconds, playlist_bytes = await request_playlist(client, playlist_url)
            bytes_received += playlist_bytes
            if init_url and not initialized:
                bytes_received += await request_media(client, init_url)
                initialized = True
            if segment_url != last_segment:
                bytes_received += await request_media(client, segment_url)
                messages_received += 1
                last_segment = segment_url
        async with state.lock:
            if state.abort_event.is_set() and index != state.trigger_viewer:
                state.result.failures += 1
                state.result.failure_phases["pre_measurement_abort"] = state.result.failure_phases.get("pre_measurement_abort", 0) + 1
                state.result.pre_measurement_abort_viewers += 1
                state.result.collateral_aborted_viewers += 1
            elif state.measurement_started and (bytes_received == steady_bytes_before or messages_received == steady_messages_before):
                state.result.failures += 1
                state.result.failure_phases["non_progressing_media"] = state.result.failure_phases.get("non_progressing_media", 0) + 1
                state.result.non_progressing_viewers += 1
            elif state.measurement_started:
                state.result.progressing += 1
                state.result.steady_video_bytes += bytes_received - steady_bytes_before
                state.result.steady_video_messages += messages_received - steady_messages_before
            state.result.received_video_bytes += bytes_received
            state.result.received_video_messages += messages_received
    except Exception as error:
        if not state.measurement_started:
            await state.fail_before_measurement(index, "connect_or_first_media", error)
        else:
            async with state.lock:
                state.result.failures += 1
                state.result.failure_phases["runtime"] = state.result.failure_phases.get("runtime", 0) + 1
                state.result.runtime_failures += 1
                key = error_text(error)
                state.result.runtime_errors[key] = state.result.runtime_errors.get(key, 0) + 1
    finally:
        await client.close()
        async with state.lock:
            state.result.completed += 1


def percentile(values: list[int], fraction: float) -> int:
    if not values:
        return 0
    return sorted(values)[int(fraction * (len(values) - 1))]


def print_results(state: State) -> None:
    result = state.result
    print(
        f"requested={state.args.viewers} allocated={result.allocated} connected={result.connected} ready={result.ready} "
        f"progressing={result.progressing} failed={result.failures} pre_measurement_failures={result.pre_measurement_failures} "
        f"trigger_failures={result.trigger_failures} pre_measurement_abort_viewers={result.pre_measurement_abort_viewers} "
        f"collateral_aborted_viewers={result.collateral_aborted_viewers} runtime_failures={result.runtime_failures} "
        f"non_progressing_viewers={result.non_progressing_viewers} received_video_bytes={result.received_video_bytes} "
        f"received_video_messages={result.received_video_messages} steady_video_bytes={result.steady_video_bytes} "
        f"steady_video_messages={result.steady_video_messages} measurement_start_unix_ns={result.measurement_start_unix_ns} "
        f"measurement_end_unix_ns={result.measurement_end_unix_ns} measurement_started={int(state.measurement_started)} "
        f"trigger_unix_ns={result.trigger_unix_ns} media_connections={result.media_connections}"
    )
    print(f"trigger_failure_phase={result.trigger_failure_phase}")
    print(f"trigger_error={result.trigger_error}")
    print(
        f"first_media_ms_p50={percentile(result.first_media_milliseconds, 0.50)} "
        f"p95={percentile(result.first_media_milliseconds, 0.95)} p99={percentile(result.first_media_milliseconds, 0.99)} "
        f"max={percentile(result.first_media_milliseconds, 1.0)}"
    )
    for phase, count in sorted(result.failure_phases.items()):
        print(f"failure_phase={phase} count={count}")
    for error, count in sorted(result.runtime_errors.items()):
        print(f"runtime_error={error} count={count}")


async def run(args: argparse.Namespace) -> int:
    state = State(args)
    tasks = []
    for index in range(args.viewers):
        tasks.append(asyncio.create_task(run_viewer(state, index)))
        if index + 1 < args.viewers:
            await asyncio.sleep(1 / args.ramp_per_second)
    await asyncio.gather(*tasks)
    print_results(state)
    return 0 if state.result.ready == args.viewers and state.result.progressing == args.viewers else 1


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--signaling-url", required=True)
    parser.add_argument("--stream-name", required=True)
    parser.add_argument("--viewers", type=int, required=True)
    parser.add_argument("--duration", type=int, required=True)
    parser.add_argument("--ramp-per-second", type=int, required=True)
    parser.add_argument("--persistent", action="store_true")
    args = parser.parse_args()
    if args.viewers <= 0 or args.duration <= 0 or args.ramp_per_second <= 0:
        parser.error("viewers, duration and ramp-per-second must be positive")
    return args


if __name__ == "__main__":
    try:
        raise SystemExit(asyncio.run(run(parse_arguments())))
    except Exception as error:
        print(f"fanout HLS generator failed: {error}", flush=True)
        raise SystemExit(2)
