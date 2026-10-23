#!/usr/bin/env python3
"""HLS fanout client with segment progress and drained media bodies."""

import argparse
import asyncio
import time
from typing import Optional
from pathlib import Path
import json
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


def playlist_paths(url, text):
    if not text.startswith("#EXTM3U"):
        raise RuntimeError("invalid playlist")
    sequence = None
    target = 2
    init_url = None
    segments = []
    for line in text.splitlines():
        if line.startswith("#EXT-X-MEDIA-SEQUENCE:"):
            sequence = int(line.split(":", 1)[1])
        elif line.startswith("#EXT-X-TARGETDURATION:"):
            target = max(1, int(line.split(":", 1)[1]))
        elif line.startswith("#EXT-X-MAP:"):
            init_url = urljoin(url, line.split('URI="', 1)[1].split('"', 1)[0])
        elif line and not line.startswith("#"):
            if sequence is None:
                raise RuntimeError("playlist has no media sequence")
            segments.append((sequence + len(segments), urljoin(url, line)))
    if "#EXT-X-ENDLIST" in text:
        raise RuntimeError("source_ended")
    if not segments:
        raise RuntimeError("playlist has no segment")
    return init_url, segments, target


class State:
    def __init__(self, args):
        self.args = args
        self.result = argparse.Namespace(media_connections=0)
        self.counters = [{"bytes": 0, "segments": 0, "playlist_bytes": 0} for _ in args.urls]
        self.first_media_ms = []
        self.ready = asyncio.Event()
        self.finished = asyncio.Event()
        self.errors = []
        self.before = None
        self.after = None
        self.start_ns = self.end_ns = 0
        self.cpu_seconds = 0

    async def measure(self):
        await self.ready.wait()
        try:
            from perf_support import wait_start
            start = await wait_start(self.args.start_file, self.args.warmup, self.args.ready_timeout)
            cpu_before = time.process_time()
            self.start_ns = time.monotonic_ns()
            self.before = [item.copy() for item in self.counters]
            print(f"phase=measurement_start monotonic_ns={self.start_ns} unix_ns={time.time_ns()}", flush=True)
            deadline = start + self.args.duration
            previous = [item["segments"] for item in self.counters]
            next_sample = start + self.args.progress_interval
            while next_sample <= deadline:
                await asyncio.sleep(max(0, next_sample - time.monotonic()))
                current = [item["segments"] for item in self.counters]
                for index, (old, new) in enumerate(zip(previous, current)):
                    if old == new:
                        self.errors.append({"viewer": index, "phase": "non_progressing_media", "monotonic_ns": time.monotonic_ns()})
                previous = current
                next_sample += self.args.progress_interval
            await asyncio.sleep(max(0, deadline - time.monotonic()))
            self.end_ns = time.monotonic_ns()
            self.cpu_seconds = time.process_time() - cpu_before
            self.after = [item.copy() for item in self.counters]
        except Exception as error:
            self.errors.append({"phase": "measurement_barrier", "error": error_text(error), "monotonic_ns": time.monotonic_ns()})
        finally:
            self.finished.set()


async def download(client, url, state, index, media=False):
    status, headers, body = await client.request("GET", url, retain_body=not media)
    if status != 200:
        raise RuntimeError(f"HTTP {status}: {url}")
    size = int(headers.get("content-length", "0"))
    if size <= 0:
        raise RuntimeError("empty response")
    if media:
        state.counters[index]["bytes"] += size
    else:
        state.counters[index]["playlist_bytes"] += size
    return body


async def run_viewer(state, index):
    client = HttpClient(state)
    started = time.monotonic()
    last_sequence = None
    try:
        url = state.args.urls[index]
        status, headers, _ = await client.request("GET", url)
        if status != 307 or "location" not in headers:
            raise RuntimeError(f"authorization_or_redirect HTTP {status}")
        url = urljoin(url, headers["location"])
        while not state.finished.is_set():
            text = (await download(client, url, state, index)).decode()
            init_url, segments, target = playlist_paths(url, text)
            if last_sequence is None:
                if init_url:
                    await download(client, init_url, state, index, True)
                segments = segments[-1:]
            elif segments[0][0] > last_sequence + 1:
                raise RuntimeError("segment_gap: viewer fell behind retention")
            for sequence, segment in segments:
                if last_sequence is not None and sequence <= last_sequence:
                    continue
                await download(client, segment, state, index, True)
                state.counters[index]["segments"] += 1
                if last_sequence is None:
                    state.first_media_ms.append(int((time.monotonic() - started) * 1000))
                    if len(state.first_media_ms) == state.args.viewers:
                        print(f"phase=media_ready ready={state.args.viewers}", flush=True)
                        state.ready.set()
                last_sequence = sequence
            await asyncio.sleep(min(target / 2, 1))
    except asyncio.CancelledError:
        raise
    except Exception as error:
        state.errors.append({"viewer": index, "phase": "runtime" if state.before is not None else "connect_or_first_media",
                             "error": error_text(error), "monotonic_ns": time.monotonic_ns()})
        state.finished.set()
    finally:
        await client.close()


async def run(args):
    state = State(args)
    viewers = []
    measurement = asyncio.create_task(state.measure())
    try:
        for index in range(args.viewers):
            viewers.append(asyncio.create_task(run_viewer(state, index)))
            if state.finished.is_set():
                break
            await asyncio.sleep(1 / args.ramp_per_second)
        await asyncio.wait_for(state.finished.wait(), args.ready_timeout + args.warmup + args.duration + args.viewers / args.ramp_per_second)
    except asyncio.TimeoutError:
        state.errors.append({"phase": "ready_timeout", "monotonic_ns": time.monotonic_ns()})
    finally:
        measurement.cancel()
        for task in viewers:
            task.cancel()
        await asyncio.gather(measurement, *viewers, return_exceptions=True)
    deltas = []
    if state.before is not None and state.after is not None:
        deltas = [{key: end[key] - begin[key] for key in begin} for begin, end in zip(state.before, state.after)]
    success = len(deltas) == args.viewers and not state.errors and all(item["segments"] > 0 for item in deltas)
    result = {"status": "PASS" if success else "FAIL", "ready": len(state.first_media_ms), "requested": args.viewers,
              "measurement_start_monotonic_ns": state.start_ns, "measurement_end_monotonic_ns": state.end_ns,
              "viewers": deltas, "first_media_ms": state.first_media_ms, "errors": state.errors,
              "media_connections": state.result.media_connections, "client_cpu_seconds": state.cpu_seconds}
    if args.output:
        args.output.write_text(json.dumps(result, indent=2) + "\n")
    print("phase=measurement requested={} ready={} progressing={} failed={} steady_media_bytes={} measurement_start_monotonic_ns={} measurement_end_monotonic_ns={}".format(
        args.viewers, len(state.first_media_ms), sum(item["segments"] > 0 for item in deltas), len(state.errors),
        sum(item["bytes"] for item in deltas), state.start_ns, state.end_ns), flush=True)
    if state.end_ns > state.start_ns:
        print(f"phase=client_resources client_cpu_cores={state.cpu_seconds / ((state.end_ns - state.start_ns) / 1e9)}", flush=True)
    for error in state.errors[:20]:
        print(json.dumps(error), flush=True)
    return 0 if success else 1


def parse_arguments():
    from perf_support import read_credentials
    parser = argparse.ArgumentParser(description="HLS unique segment download and per-viewer progress check")
    parser.add_argument("--hls-urls-file", type=Path, required=True)
    parser.add_argument("--viewers", type=int, required=True)
    parser.add_argument("--duration", type=float, required=True)
    parser.add_argument("--warmup", type=float, default=5)
    parser.add_argument("--ramp-per-second", type=int, default=50)
    parser.add_argument("--ready-timeout", type=float, default=60)
    parser.add_argument("--progress-interval", type=float, default=6, help="must exceed playlist target duration")
    parser.add_argument("--start-file", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--persistent", action="store_true")
    args = parser.parse_args()
    if min(args.viewers, args.duration, args.ramp_per_second, args.ready_timeout, args.progress_interval) <= 0 or args.warmup < 0:
        parser.error("positive viewers, duration, ramp, timeouts and nonnegative warmup required")
    if args.duration < args.progress_interval:
        parser.error("duration must cover at least one progress interval")
    try:
        args.urls = read_credentials(args.hls_urls_file, args.viewers)
    except ValueError as error:
        parser.error(str(error))
    return args


if __name__ == "__main__":
    raise SystemExit(asyncio.run(run(parse_arguments())))
