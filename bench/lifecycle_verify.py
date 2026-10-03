#!/usr/bin/env python3
"""Public protocol lifecycle verification using the existing media clients."""

import argparse
import asyncio
import contextlib
import http.client
import json
import os
import re
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path
from types import SimpleNamespace

import gb_network
import http_flv_fanout
from fanout_support import benchmark_head, parse_phase, proc_snapshot, stop_process, wait_for_listener, wait_for_phase, wait_for_stream


def request(port, method, path, body=None, content_type="application/json"):
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
    try:
        data = json.dumps(body).encode() if isinstance(body, dict) else body
        connection.request(method, path, data, {"Content-Type": content_type} if data is not None else {})
        response = connection.getresponse()
        return response.status, dict(response.getheaders()), response.read()
    finally:
        connection.close()


def snapshot(pid):
    result = proc_snapshot(pid)
    result["thread_count"] = len(result.pop("threads"))
    result["time"] = time.monotonic()
    for line in Path(f"/proc/{pid}/status").read_text().splitlines():
        if line.startswith(("VmSize:", "VmHWM:")):
            result[line.split(":")[0]] = int(line.split()[1])
    result["loopback_tx_bytes"] = int(Path("/sys/class/net/lo/statistics/tx_bytes").read_text())
    return result


async def http_wave(port, name, count, duration):
    window = {"start": float("inf"), "end": float("inf")}
    ready = [asyncio.Event() for _ in range(count)]
    tasks = [asyncio.create_task(http_flv_fanout.viewer(i, 1, "127.0.0.1", port, window, ready[i], name)) for i in range(count)]
    try:
        await asyncio.wait_for(asyncio.gather(*(event.wait() for event in ready)), 10)
        window["start"] = time.monotonic()
        window["end"] = window["start"] + duration
        results = await asyncio.wait_for(asyncio.gather(*tasks), duration + 5)
        assert all(item["established"] and item["bytes"] > 10000 and item["error"] is None for item in results), results
        return results
    finally:
        for task in tasks:
            task.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)


class Run:
    def __init__(self, args):
        self.args = args
        self.output = args.output
        self.output.mkdir(parents=True, exist_ok=True)
        self.stack = contextlib.ExitStack()
        self.sequence = 0
        self.records = []
        self.host = "127.0.0.1"
        self.rtmp_port = args.port_base
        self.rtsp_port = args.port_base + 1
        self.http_port = args.port_base + 2
        self.server = self.launch("server", [args.build_dir / "media_server", "--threads", "6", "--rtmp-port", self.rtmp_port,
                                           "--rtsp-port", self.rtsp_port, "--http-port", self.http_port,
                                           "--bind-address", self.host, "--webrtc-address", self.host])
        try:
            wait_for_listener(self.host, self.rtmp_port)
            self.idle = snapshot(self.server.pid)
        except BaseException:
            self.stack.close()
            raise

    def launch(self, label, command, stdin=None, env=None):
        self.sequence += 1
        path = self.output / f"{self.sequence:04d}-{label}.log"
        log = self.stack.enter_context(path.open("w"))
        process = subprocess.Popen([str(item) for item in command], stdin=stdin, env=env, stdout=log, stderr=subprocess.STDOUT)
        process.log_path = path
        self.stack.callback(stop_process, process)
        self.records.append({"label": label, "command": [str(item) for item in command], "log": path.name})
        return process

    def close(self):
        self.stack.close()
        log = self.server.log_path.read_text()
        assert self.server.returncode == 0, (self.server.returncode, log[-4000:])
        for record in self.records:
            log = (self.output / record["log"]).read_text()
            assert not re.search(r"ERROR: (AddressSanitizer|LeakSanitizer)|runtime error:|WARNING: ThreadSanitizer|write queue full", log), log[-4000:]

    @contextlib.contextmanager
    def publisher(self, protocol, name="live/perf0", reset=False):
        if protocol == "whip":
            assert name == "live/perf0"
            command = [self.args.client_dir / "whip_publisher", "--whip-base-url", f"http://{self.host}:{self.http_port}/publish/whip/",
                       "--stream-prefix", "live/perf", "--sources", "1", "--io-threads", "2", "--ramp-per-second", "100",
                       "--bitrate", "2000000", "--duration", "3600"]
        else:
            command = [self.args.ffmpeg, "-hide_banner", "-loglevel", "error", "-stream_loop", "-1", "-re", "-i", self.args.fixture,
                       "-c", "copy"]
            command += ["-f", "flv", f"rtmp://{self.host}:{self.rtmp_port}/{name}"] if protocol == "rtmp" else [
                "-f", "rtsp", "-rtsp_transport", self.args.rtsp_publish_transport, f"rtsp://{self.host}:{self.rtsp_port}/{name}"]
        environment = {**os.environ, "LD_PRELOAD": str(self.output / "reset_peer.so")} if reset else None
        process = self.launch(f"publish-{protocol}", command, env=environment)
        try:
            wait_for_stream(self.host, self.http_port, name)
            yield process
        finally:
            if protocol == "whip":
                sessions = re.findall(r"whip session created (\w+) stream " + re.escape(name), self.server.log_path.read_text())
                assert sessions, self.server.log_path.read_text()[-2000:]
                path = "/publish/whip/session/" + sessions[-1]
                assert request(self.http_port, "DELETE", path)[0] == 204
                assert request(self.http_port, "DELETE", path)[0] == 404
            if reset and process.poll() is None:
                process.kill()
                process.wait(timeout=5)
            else:
                stop_process(process)
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                with socket.create_connection((self.host, self.http_port), timeout=2) as probe:
                    probe.sendall(f"GET /{name}.flv HTTP/1.1\r\nHost: localhost\r\n\r\n".encode())
                    status = probe.recv(64)
                if status.startswith(b"HTTP/1.1 404"):
                    break
                time.sleep(0.05)
            assert status.startswith(b"HTTP/1.1 404"), "ended publisher remains in source registry"

    @contextlib.contextmanager
    def pair(self, transport="udp", index=0):
        args = SimpleNamespace(host=self.host, http_port=self.http_port, transport=transport, tcp_port_base=self.args.port_base + 3)
        pair = gb_network.create_pair(args, index)
        try:
            wait_for_stream(self.host, self.http_port, pair[0])
            yield pair[0]
        finally:
            name, receiver, sender, sender_name = pair
            for path, body in (("sender", {"stream_id": sender, "stream_name": "live/perf0", "sender_id": sender_name}),
                               ("receiver", {"stream_id": receiver, "stream_name": name})):
                status = request(self.http_port, "POST", f"/gb28181/{path}/delete", body)[0]
                expected = (204, 404) if path == "receiver" and transport != "udp" else (204,)
                assert status in expected, (path, transport, status)
                assert request(self.http_port, "POST", f"/gb28181/{path}/delete", body)[0] == 404
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                with socket.create_connection((self.host, self.http_port), timeout=2) as probe:
                    probe.sendall(f"GET /{name}.flv HTTP/1.1\r\nHost: localhost\r\n\r\n".encode())
                    status = probe.recv(64)
                if status.startswith(b"HTTP/1.1 404"):
                    break
                time.sleep(0.05)
            assert status.startswith(b"HTTP/1.1 404"), "deleted GB receiver remains in source registry"

    def clients(self, name, count=1, duration=3, hls=True):
        common = ["--viewers", count, "--ramp-per-second", "100", "--warmup", "0", "--duration", duration]
        commands = {
            "rtmp": [self.args.client_dir / "rtmp_fanout", "--stream-name", name, "--media-host", self.host, "--media-port", self.rtmp_port, *common],
            "rtsp": [self.args.client_dir / "rtsp_fanout", "--stream-name", name, "--media-host", self.host, "--media-port", self.rtsp_port, *common],
            "whep": [self.args.client_dir / "whep_fanout", "--whep-url", f"http://{self.host}:{self.http_port}/play/whep/{name}", "--io-threads", "4", *common],
        }
        if hls:
            commands["hls"] = [sys.executable, Path(__file__).with_name("hls_client.py"), "--hls-url",
                               f"http://{self.host}:{self.http_port}/play/hls/{name}/index.m3u8", *common]
        return {protocol: self.launch(protocol, command) for protocol, command in commands.items()}

    def finish(self, clients, count):
        result = {}
        for protocol, process in clients.items():
            assert process.wait(timeout=20) == 0, (protocol, process.log_path.read_text())
            lines = process.log_path.read_text().splitlines()
            measurement = next(parse_phase(line) for line in lines if line.startswith("phase=measurement "))
            assert measurement["progressing"] == count and measurement.get("failed", 0) == 0, measurement
            if protocol in ("rtmp", "rtsp"):
                assert measurement["steady_video_bytes"] > 0 and measurement["steady_audio_bytes"] > 0, measurement
            if protocol == "whep":
                assert measurement["audio_packets"] > 0 and measurement["video_packets"] > 0, measurement
                assert measurement["runtime_failures"] == 0 and measurement["unprotect_failures"] == 0, measurement
                disconnect = next(parse_phase(line) for line in lines if line.startswith("phase=disconnect "))
                assert disconnect["stopped"] == count and disconnect["removed"] == count, disconnect
            if protocol == "hls":
                measurement["session_playlists"] = [parse_phase(line)["playlist"] for line in lines if line.startswith("phase=session ")]
            result[protocol] = measurement
        return result

    def wave(self, name="live/perf0", count=1, duration=3, hls=True):
        clients = self.clients(name, count, duration, hls)
        http = asyncio.run(http_wave(self.http_port, name, count, duration))
        return {**self.finish(clients, count), "http-flv": http, "resources": snapshot(self.server.pid)}

    def hls_location(self, name):
        status, headers, _ = request(self.http_port, "GET", f"/play/hls/{name}/index.m3u8")
        assert status == 307, status
        return headers["Location"]

    def smoke(self):
        result = {}
        for protocol in ("rtmp", "rtsp", "whip"):
            with self.publisher(protocol):
                result[protocol] = {"single": self.wave(), "multiple": self.wave(count=4)}
                if protocol != "whip":
                    for output, port in (("rtmp", self.rtmp_port), ("rtsp", self.rtsp_port), ("http", self.http_port), ("hls", self.http_port)):
                        command = [self.args.ffmpeg, "-hide_banner", "-loglevel", "info"]
                        if output == "rtsp":
                            command += ["-rtsp_transport", "tcp", "-timeout", "5000000"]
                        elif output != "hls":
                            command += ["-rw_timeout", "5000000"]
                        suffix = ".flv" if output == "http" else ""
                        url = f"http://{self.host}:{port}/play/hls/live/perf0/index.m3u8" if output == "hls" else f"{output}://{self.host}:{port}/live/perf0{suffix}"
                        command += ["-i", url, "-map", "0:v:0", "-map", "0:a:0", "-t", "2", "-f", "null", "-"]
                        probe = self.launch(f"decode-{output}", command)
                        assert probe.wait(timeout=20) == 0, probe.log_path.read_text()
                        text = probe.log_path.read_text()
                        assert "Video: h264" in text and "Audio: aac" in text and "time=00:00:02" in text, text
                for transport in ("udp", "tcp_sender_active", "tcp_sender_passive"):
                    with self.pair(transport) as name:
                        result[protocol][transport] = self.wave(name, hls=False)
        return result

    def replacement(self):
        result = {}
        for protocol in self.args.inputs:
            entries = []
            with self.publisher("rtmp", "live/perf0") if protocol == "gb" else contextlib.nullcontext():
                for generation in range(self.args.generations):
                    name = "relay/perf0" if protocol == "gb" else "live/perf0"
                    with self.pair(self.args.gb_transport) if protocol == "gb" else self.publisher(protocol):
                        old = gb_network.open_viewer(self.host, self.http_port, name)
                        self.stack.callback(old.close)
                        try:
                            location = self.hls_location(name)
                            status, _, body = request(self.http_port, "GET", location)
                            assert status == 200 and b"#EXTM3U" in body
                            entry = self.wave(name, duration=2, hls=False)
                            entry["generation"] = generation
                            entry["hls_location"] = location
                        finally:
                            old.settimeout(5)
                    try:
                        while old.recv(65536):
                            pass
                    finally:
                        old.close()
                    status, _, body = request(self.http_port, "GET", location)
                    assert status in (200, 404), (status, body)
                    if status == 200:
                        assert b"#EXT-X-ENDLIST" in body, body
                    assert all(prior["hls_location"] != location for prior in entries), "HLS session reused across source generations"
                    entries.append(entry)
                    print(f"replacement {protocol} generation={generation + 1} PASS", flush=True)
            result[protocol] = entries
        return result

    def churn(self):
        result = {"waves": [], "whip": [], "gb": [], "iterations_per_protocol": self.args.iterations, "batch": self.args.batch}
        with self.publisher("rtmp"):
            for iteration in range(self.args.iterations // self.args.batch):
                result["waves"].append(self.wave(count=self.args.batch))
                print(f"viewer churn {(iteration + 1) * self.args.batch} PASS", flush=True)
            for iteration in range(self.args.iterations // self.args.batch):
                command = [self.args.client_dir / "whip_publisher", "--whip-base-url", f"http://{self.host}:{self.http_port}/publish/whip/",
                           "--stream-prefix", "churn/perf", "--sources", self.args.batch, "--io-threads", "4",
                           "--ramp-per-second", "100", "--bitrate", "100000", "--duration", "1"]
                process = self.launch("whip-churn", command)
                assert process.wait(timeout=30) == 0, process.log_path.read_text()
                result["whip"].append(process.log_path.read_text())
            for iteration in range(self.args.iterations // self.args.batch):
                with contextlib.ExitStack() as pairs:
                    names = [pairs.enter_context(self.pair(index=index)) for index in range(self.args.batch)]
                    async def receive():
                        return await asyncio.gather(*(http_wave(self.http_port, name, 1, 1) for name in names))
                    asyncio.run(receive())
                result["gb"].append(snapshot(self.server.pid))
                print(f"GB churn {(iteration + 1) * self.args.batch} PASS", flush=True)
        baseline = snapshot(self.server.pid)
        deadline = time.monotonic() + 45
        while time.monotonic() < deadline:
            current = snapshot(self.server.pid)
            if current["fd"] <= self.idle["fd"] + 2:
                break
            time.sleep(0.2)
        result["after_publish"] = baseline
        result["settled"] = current
        assert current["fd"] <= self.idle["fd"] + 2 and current["thread_count"] == self.idle["thread_count"], current
        playlists = [url for wave in result["waves"] for url in wave["hls"]["session_playlists"]]
        assert len(playlists) == self.args.iterations, len(playlists)
        for url in playlists:
            path = url.split(f":{self.http_port}", 1)[1]
            status, _, body = request(self.http_port, "GET", path)
            assert status == 403 and body == b"invalid hls session\n", (status, body)
        result["expired_hls_sessions"] = len(playlists)
        result["expired_hls_status"] = 403
        result["idle"] = self.idle
        return result

    def soak(self):
        started = time.monotonic()
        result = {"samples": [], "generations": 0, "waves": [], "gb_waves": [], "measurements": [], "idle": self.idle}
        while time.monotonic() - started < self.args.duration:
            with self.publisher("rtmp"), self.pair(self.args.gb_transport) as relay_name:
                result["generations"] += 1
                clients = self.clients("live/perf0", 4, 25)
                async def monitor():
                    async def samples():
                        deadline = time.monotonic() + 25
                        while time.monotonic() < deadline:
                            sample = snapshot(self.server.pid)
                            assert sample["rss_kib"] < 2 * 1024 * 1024, sample
                            if result["samples"]:
                                previous = result["samples"][-1]
                                assert (sample["cpu"] - previous["cpu"]) / (sample["time"] - previous["time"]) < 5.4, sample
                            result["samples"].append(sample)
                            await asyncio.sleep(1)
                    received, gb_received, _ = await asyncio.gather(
                        http_wave(self.http_port, "live/perf0", 4, 25),
                        http_wave(self.http_port, relay_name, 1, 25), samples())
                    return received, gb_received
                received, gb_received = asyncio.run(monitor())
                result["waves"].append(received)
                result["gb_waves"].append(gb_received)
                result["measurements"].append(self.finish(clients, 4))
                result["samples"].append(snapshot(self.server.pid))
            result["settled"] = snapshot(self.server.pid)
            (self.output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
            print(f"soak seconds={time.monotonic() - started:.1f} generation={result['generations']} PASS", flush=True)
        result["duration_seconds"] = time.monotonic() - started
        return result

    def disconnect(self):
        subprocess.run(["cc", "-shared", "-fPIC", "-Wall", "-Wextra", "-Werror",
                        Path(__file__).resolve().parents[1] / "tests/reset_peer.c", "-o", self.output / "reset_peer.so", "-ldl"], check=True)
        result = {"cases": [], "before": self.idle}
        for protocol in ("rtmp", "rtsp"):
            with self.publisher(protocol, reset=True):
                result["cases"].append({"publisher_reset": protocol, "media": self.wave(hls=False)})
        with self.publisher("rtmp"):
            active_idle = snapshot(self.server.pid)
            for protocol, port in (("rtmp", self.rtmp_port), ("rtsp", self.rtsp_port)):
                progress = self.output / f"{protocol}-reset-progress.txt"
                command = [self.args.ffmpeg, "-hide_banner", "-loglevel", "error", "-progress", progress]
                if protocol == "rtsp":
                    command += ["-rtsp_transport", "tcp"]
                command += ["-i", f"{protocol}://{self.host}:{port}/live/perf0", "-c", "copy", "-f", "null", "-"]
                viewer = self.launch(f"{protocol}-viewer-reset", command, env={**os.environ, "LD_PRELOAD": str(self.output / "reset_peer.so")})
                deadline = time.monotonic() + 10
                while time.monotonic() < deadline:
                    if progress.exists() and any(int(value) > 0 for value in re.findall(r"frame=(\d+)", progress.read_text())):
                        break
                    assert viewer.poll() is None, viewer.log_path.read_text()
                    time.sleep(0.05)
                assert progress.exists() and any(int(value) > 0 for value in re.findall(r"frame=(\d+)", progress.read_text()))
                viewer.kill()
                viewer.wait(timeout=5)
                result["cases"].append({"viewer_tcp_reset": protocol, "real_frames": True})
            pair = gb_network.create_pair(SimpleNamespace(host=self.host, http_port=self.http_port, transport="udp", tcp_port_base=self.args.port_base + 3), 0)
            name, receiver, sender, sender_name = pair
            wait_for_stream(self.host, self.http_port, name)
            asyncio.run(http_wave(self.http_port, name, 1, 1))
            connection = gb_network.open_viewer(self.host, self.http_port, name)
            try:
                sender_body = {"stream_id": sender, "stream_name": "live/perf0", "sender_id": sender_name}
                assert request(self.http_port, "POST", "/gb28181/sender/delete", sender_body)[0] == 204
                connection.settimeout(0.3)
                try:
                    while connection.recv(65536):
                        pass
                except socket.timeout:
                    connection.settimeout(1)
                    try:
                        assert connection.recv(65536) == b"", "UDP receiver kept producing media after peer disappeared"
                    except socket.timeout:
                        pass
                receiver_body = {"stream_id": receiver, "stream_name": name}
                status = request(self.http_port, "POST", "/gb28181/receiver/delete", receiver_body)[0]
                assert status in (204, 404), status
                assert request(self.http_port, "POST", "/gb28181/receiver/delete", receiver_body)[0] == 404
                connection.settimeout(5)
                while connection.recv(65536):
                    pass
                ports = re.findall(r"gb28181 udp session started stream relay/perf0 rtp_port (\d+) rtcp_port (\d+)", self.server.log_path.read_text())[-1]
                for port in ports:
                    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as released:
                        released.bind((self.host, int(port)))
                result["cases"].append({"gb_udp_peer_disappeared": "PASS", "receiver_delete_status": status, "ports_released": ports})
            finally:
                connection.close()
            for port, data in ((self.http_port, b"GET /live/perf0.flv HTTP/1.1\r\nHost: localhost\r\n\r\n"),
                               (self.rtmp_port, b"\x03" + bytes(1536))):
                with socket.create_connection((self.host, port), timeout=5) as connection:
                    connection.sendall(data)
                    assert connection.recv(4096)
                    connection.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
                result["cases"].append({"viewer_reset_port": port})
            with socket.create_connection((self.host, self.rtsp_port), timeout=5) as connection:
                stream = connection.makefile("rb")
                try:
                    for sequence, method in enumerate(("DESCRIBE", "SETUP"), 1):
                        target = f"rtsp://{self.host}:{self.rtsp_port}/live/perf0" + ("/trackID=1" if method == "SETUP" else "")
                        headers = "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n" if method == "SETUP" else "Accept: application/sdp\r\n"
                        connection.sendall(f"{method} {target} RTSP/1.0\r\nCSeq: {sequence}\r\n{headers}\r\n".encode())
                        assert stream.readline().startswith(b"RTSP/1.0 200")
                        response_headers = {}
                        while (line := stream.readline()) != b"\r\n":
                            key, value = line.decode().split(":", 1)
                            response_headers[key.lower()] = value.strip()
                        stream.read(int(response_headers.get("content-length", "0")))
                    connection.settimeout(0.2)
                    try:
                        data = connection.recv(1)
                        raise AssertionError(f"SETUP without PLAY received media: {data!r}")
                    except socket.timeout:
                        pass
                finally:
                    stream.close()
            result["cases"].append({"rtsp_setup_without_play": "PASS"})
            resources = []
            for direction, prefix in (("play", "whep"), ("publish", "whip")):
                for expected in ("established", "dtls_timeout"):
                    peer = self.launch(f"{prefix}-{expected}", [self.args.client_dir / "webrtc_disconnect", direction, expected], stdin=subprocess.PIPE)
                    self.stack.callback(peer.stdin.close)
                    wait_for_phase(peer.log_path, peer, "offer_ready", 5)
                    offer = peer.log_path.read_text().split("phase=offer_ready")[0]
                    path = f"/{'play' if prefix == 'whep' else 'publish'}/{prefix}/" + ("live/perf0" if prefix == "whep" else f"disconnect/{expected}")
                    status, headers, answer = request(self.http_port, "POST", path, offer, "application/sdp")
                    assert status == 201, (status, answer)
                    candidate = re.search(rb"a=candidate:[^\r\n]*? (\d+) typ host", answer)
                    assert candidate, answer
                    port = int(candidate[1])
                    resources.append((prefix, headers["Location"], port, time.monotonic()))
                    if expected == "dtls_timeout":
                        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as proxy:
                            proxy.bind((self.host, 0))
                            proxy.settimeout(0.1)
                            modified = answer[:candidate.start(1)] + str(proxy.getsockname()[1]).encode() + answer[candidate.end(1):]
                            peer.stdin.write(modified)
                            peer.stdin.close()
                            client = None
                            while peer.poll() is None:
                                try:
                                    packet, remote = proxy.recvfrom(4096)
                                except socket.timeout:
                                    continue
                                if len(packet) >= 20 and packet[4:8] == b"\x21\x12\xa4\x42":
                                    if remote == (self.host, port):
                                        assert client is not None
                                        proxy.sendto(packet, client)
                                    else:
                                        client = remote
                                        proxy.sendto(packet, (self.host, port))
                    else:
                        peer.stdin.write(answer)
                        peer.stdin.close()
                    assert peer.wait(timeout=5) == 0, peer.log_path.read_text()
                    local_port = int(re.search(r"local_port=(\d+)", peer.log_path.read_text())[1])
                    quarantine = self.stack.enter_context(socket.socket(socket.AF_INET, socket.SOCK_DGRAM))
                    quarantine.bind((self.host, local_port))
                    result["cases"].append({"webrtc": prefix, "case": expected, "port": port})
            for prefix, location, port, created in resources:
                deadline = created + 36
                while time.monotonic() < deadline:
                    if prefix == "whip":
                        session = location.rsplit("/", 1)[1]
                        if f"session shutdown {session}" in self.server.log_path.read_text():
                            break
                    elif request(self.http_port, "GET", location)[0] == 404:
                        break
                    time.sleep(0.1)
                assert request(self.http_port, "DELETE", location)[0] == 404, location
                assert time.monotonic() < deadline, "WebRTC resource missed existing lifecycle deadline"
                session = location.rsplit("/", 1)[1]
                assert self.server.log_path.read_text().count(f"session shutdown {session}") == 1, location
                assert f"ice connected session {session}" in self.server.log_path.read_text(), location
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as released:
                    released.bind((self.host, port))
            result["after"] = snapshot(self.server.pid)
            result["recovery"] = self.wave(hls=False)
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                settled = snapshot(self.server.pid)
                if settled["fd"] <= active_idle["fd"] + 2:
                    break
                time.sleep(0.05)
            assert settled["fd"] <= active_idle["fd"] + 2 and settled["thread_count"] == active_idle["thread_count"], settled
            result["active_idle"] = active_idle
            result["settled"] = settled
        return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("suite", choices=("smoke", "replacement", "churn", "soak", "disconnect"))
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--client-dir", type=Path)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--ffmpeg", type=Path, default=Path("/home/gyl/bin/ffmpeg"))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--port-base", type=int, default=21930)
    parser.add_argument("--generations", type=int, default=20)
    parser.add_argument("--inputs", nargs="+", choices=("rtmp", "rtsp", "whip", "gb"), default=["rtmp", "rtsp", "whip", "gb"])
    parser.add_argument("--gb-transport", choices=("udp", "tcp_sender_active", "tcp_sender_passive"), default="udp")
    parser.add_argument("--rtsp-publish-transport", choices=("tcp", "udp"), default="tcp")
    parser.add_argument("--iterations", type=int, default=300)
    parser.add_argument("--batch", type=int, default=20)
    parser.add_argument("--duration", type=int, default=600)
    args = parser.parse_args()
    args.client_dir = args.client_dir or args.build_dir
    if min(args.generations, args.iterations, args.batch, args.duration) < 1 or args.iterations % args.batch:
        parser.error("positive counts and iterations divisible by batch are required")
    run = Run(args)
    try:
        result = getattr(run, args.suite)()
    finally:
        try:
            run.close()
        finally:
            (args.output / "commands.json").write_text(json.dumps(run.records, indent=2) + "\n")
    config = {key: str(value) if isinstance(value, Path) else value for key, value in vars(args).items()}
    (args.output / "result.json").write_text(json.dumps({"head": benchmark_head(), "suite": args.suite, "config": config, "result": result}, indent=2) + "\n")
    print(f"{args.suite}: PASS", flush=True)


if __name__ == "__main__":
    main()
