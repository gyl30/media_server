import argparse
import pathlib
import socketserver
import struct
import subprocess
import tempfile
import threading
import time
import unittest
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class NativeCredentials(unittest.TestCase):
    def test_whep_multisource_credentials_reach_protocol_client(self):
        with tempfile.TemporaryDirectory() as directory:
            urls = pathlib.Path(directory) / "urls.txt"
            urls.write_text("http://127.0.0.1:1/play/whep/a/token-a\nhttp://127.0.0.1:1/play/whep/b/token-b\n")
            result = subprocess.run(
                [str(BUILD / "whep_fanout"), "--whep-urls-file", str(urls), "--sources", "2",
                 "--viewers", "2", "--duration", "1"], capture_output=True, text=True, timeout=10,
            )
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn("establishment_failures=2", result.stdout)

    def test_help_needs_no_credentials_or_service(self):
        for protocol in ("rtmp", "rtsp", "http_flv"):
            result = subprocess.run([str(BUILD / f"{protocol}_fanout"), "--help"], capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("--play-paths-file", result.stdout)

    def test_distinct_paths_reach_protocol_client(self):
        for protocol in ("rtmp", "rtsp", "http_flv"):
            with self.subTest(protocol=protocol), tempfile.TemporaryDirectory() as directory:
                paths = pathlib.Path(directory) / "paths.txt"
                paths.write_text("stream-a/token-a\nstream-b/token-b\n")
                result = subprocess.run(
                    [str(BUILD / f"{protocol}_fanout"), "--play-paths-file", str(paths),
                     "--viewers", "2", "--media-port", "1", "--duration", "1"],
                    capture_output=True, text=True, timeout=10,
                )
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn("trigger_failure_phase=connect_or_handshake", result.stdout)

    def test_duplicate_paths_are_rejected_before_connect(self):
        for protocol in ("rtmp", "rtsp", "http_flv"):
            with self.subTest(protocol=protocol), tempfile.TemporaryDirectory() as directory:
                paths = pathlib.Path(directory) / "paths.txt"
                paths.write_text("stream/token\nstream/token\n")
                result = subprocess.run(
                    [str(BUILD / f"{protocol}_fanout"), "--play-paths-file", str(paths),
                     "--viewers", "2", "--duration", "1"],
                    capture_output=True, text=True, timeout=10,
                )
                self.assertEqual(result.returncode, 2)
                self.assertIn("distinct", result.stderr)


class RtspReadiness(unittest.TestCase):
    def run_client(self, tracks, duration=1, stall_after=None, warmup=0, extra_args=(), sequence_gap=False):
        stopped = threading.Event()
        class Handler(socketserver.StreamRequestHandler):
            def handle(self):
                while True:
                    first = self.rfile.readline()
                    if not first:
                        return
                    headers = {}
                    while (line := self.rfile.readline()) != b"\r\n":
                        if not line:
                            return
                        key, value = line.decode().split(":", 1)
                        headers[key.lower()] = value.strip()
                    body = b""
                    extra = ""
                    method = first.split()[0]
                    if method == b"DESCRIBE":
                        body = b"v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=test\r\nt=0 0\r\n"
                        for channel, kind, size in tracks or [(0, "video", 0)]:
                            codec = "H264/90000" if kind == "video" else "MPEG4-GENERIC/48000/2"
                            body += (f"m={kind} 0 RTP/AVP {96 + channel}\r\na=rtpmap:{96 + channel} {codec}\r\n"
                                     f"a=control:track{channel}\r\n").encode()
                        extra = "Content-Type: application/sdp\r\n"
                    elif method == b"SETUP":
                        extra = "Session: test\r\nTransport: " + headers["transport"] + "\r\n"
                    self.wfile.write((f"RTSP/1.0 200 OK\r\nCSeq: {headers['cseq']}\r\n"
                                      f"{extra}Content-Length: {len(body)}\r\n\r\n").encode() + body)
                    if method == b"PLAY" and tracks:
                        sequence = 0
                        started = time.monotonic()
                        try:
                            while not stopped.is_set():
                                if stall_after is not None and time.monotonic() - started > stall_after:
                                    time.sleep(0.05)
                                    continue
                                for channel, kind, size in tracks:
                                    packet = struct.pack("!BBHII", 0x80, 96 + channel, (sequence + (sequence >= 4 and sequence_gap)) & 65535, sequence * 3000, channel + 1)
                                    packet += bytes(size)
                                    self.wfile.write(struct.pack("!BBH", 36, channel, len(packet)) + packet)
                                sequence += 1
                                time.sleep(0.05)
                        except (BrokenPipeError, ConnectionResetError):
                            return

        with socketserver.ThreadingTCPServer(("127.0.0.1", 0), Handler) as server:
            server.daemon_threads = True
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            try:
                result = subprocess.run(
                    [str(BUILD / "rtsp_fanout"), "--stream-name", "stream/token", "--viewers", "1",
                     "--media-port", str(server.server_address[1]), "--ready-timeout", "1", "--warmup", str(warmup), "--progress-interval", "1", "--duration", str(duration),
                     *(["--video-only"] if not any(kind == "audio" for _, kind, _ in tracks) else []), *extra_args],
                    capture_output=True, text=True, timeout=duration + warmup + 3,
                )
                return result
            finally:
                stopped.set()
                server.shutdown()
                thread.join(timeout=2)

    def test_successful_play_without_media_times_out(self):
        result = self.run_client([])
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("timed out", result.stdout.lower())

    def test_video_only_source_is_ready(self):
        result = self.run_client([(0, "video", 100)])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_sdp_order_defines_track_counters(self):
        result = self.run_client([(0, "audio", 10), (2, "video", 100)])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        measurement = next(line for line in result.stdout.splitlines() if line.startswith("phase=measurement "))
        counters = dict(value.split("=", 1) for value in measurement.split())
        self.assertGreater(int(counters["steady_video_bytes"]), int(counters["steady_audio_bytes"]))

    def test_media_stall_is_not_hidden_by_initial_growth(self):
        result = self.run_client([(0, "video", 100)], duration=6, stall_after=0.25)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("failure_phase=non_progressing_media", result.stdout)


    def test_stall_during_warmup_has_bounded_failure(self):
        result = self.run_client([(0, "video", 100)], stall_after=0.1, warmup=1)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("failure_phase=non_progressing_media", result.stdout)


    def test_rtp_gap_is_not_healthy(self):
        result = self.run_client([(0, "video", 100)], sequence_gap=True)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)

    def test_external_start_controls_measurement_window(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "start.txt"
            start = time.monotonic_ns() + 500_000_000
            path.write_text(str(start))
            result = self.run_client([(0, "video", 100)], extra_args=("--start-file", str(path)))
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            phase = next(line for line in result.stdout.splitlines() if line.startswith("phase=measurement_start "))
            fields = dict(value.split("=", 1) for value in phase.split())
            actual = int(fields["monotonic_ns"])
            self.assertGreaterEqual(actual, start)
            self.assertLess(actual - start, 200_000_000)

    def test_missing_external_start_has_bounded_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "missing.txt"
            result = self.run_client([(0, "video", 100)], extra_args=("--start-file", str(path)))
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn("start-file timed out", result.stdout)


class FlvHealth(unittest.TestCase):
    def run_client(self, media, reverse=False):
        stopped = threading.Event()

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def do_GET(self):
                self.send_response(200)
                self.send_header("Transfer-Encoding", "chunked")
                self.send_header("Connection", "close")
                self.end_headers()

                def send_chunk(data):
                    self.wfile.write(f"{len(data):x}\r\n".encode() + data + b"\r\n")

                count = 0
                def tag(kind, body):
                    stamp = (0 if reverse and count >= 5 else 100).to_bytes(4, "big")
                    header = bytes([kind]) + len(body).to_bytes(3, "big") + stamp[1:] + stamp[:1] + bytes(3)
                    return header + body + (11 + len(body)).to_bytes(4, "big")

                try:
                    send_chunk(b"FLV\x01\x05\x00\x00\x00\x09\x00\x00\x00\x00")
                    while not stopped.is_set():
                        if media:
                            data = tag(9, b"\x27\x01\x00\x00\x00\x00\x00\x00\x02\x41\x80")
                            data += tag(8, b"\xaf\x01\x21\x10\x04\x60")
                        else:
                            data = tag(9, b"\x17\x00\x00\x00\x00\x01\x42\x00\x1f")
                            data += tag(8, b"\xaf\x00\x11\x90")
                        send_chunk(data)
                        count += 1
                        time.sleep(0.03)
                    self.wfile.write(b"0\r\n\r\n")
                except (BrokenPipeError, ConnectionResetError):
                    pass

            def log_message(self, *_):
                pass

        with ThreadingHTTPServer(("127.0.0.1", 0), Handler) as server:
            server.daemon_threads = False
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            try:
                return subprocess.run(
                    [str(BUILD / "http_flv_fanout"), "--stream-name", "stream/token.flv", "--viewers", "1",
                     "--media-port", str(server.server_port), "--ready-timeout", "1", "--warmup", "0", "--duration", "1"],
                    capture_output=True, text=True, timeout=5,
                )
            finally:
                stopped.set()
                server.shutdown()
                thread.join(timeout=2)

    def test_chunked_media_tags_are_healthy(self):
        result = self.run_client(True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("progressing=1", result.stdout)

    def test_sequence_headers_are_not_media_progress(self):
        result = self.run_client(False)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("timed out", result.stdout.lower())
        self.assertIn("received_video_messages=0", result.stdout)


    def test_media_timestamp_reversal_fails(self):
        result = self.run_client(True, reverse=True)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)


class WorkloadSchedule(unittest.TestCase):
    def test_search_rechecks_health_and_server_failure(self):
        sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "bench"))
        from perf_capacity import search
        calls = []
        def execute(count):
            calls.append(count)
            return "HEALTHY" if count <= 800 else "SERVER_UNSTABLE"
        result = search(execute, 500, 500, 100, 2000)
        self.assertEqual(result["status"], "PASS")
        self.assertEqual((result["healthy_capacity"], result["first_failed"]), (800, 900))
        self.assertEqual(calls, [500, 1000, 700, 800, 900, 800, 800, 800, 900, 900])
        self.assertEqual(result["overload_diagnostic_counts"], [900, 1000, 1200])

    def test_client_limit_cannot_be_server_boundary(self):
        sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "bench"))
        from perf_capacity import search
        result = search(lambda count: "HEALTHY" if count == 500 else "CLIENT_RESOURCE", 500, 500, 100, 2000)
        self.assertEqual(result["status"], "INCONCLUSIVE")
        self.assertNotIn("healthy_capacity", result)

    def test_whip_rejects_duplicate_before_fixture_read(self):
        with tempfile.TemporaryDirectory() as directory:
            urls = pathlib.Path(directory) / "urls"
            urls.write_text("http://127.0.0.1/publish/whip/token\n" * 2)
            result = subprocess.run([str(BUILD / "whip_publisher"), "--whip-urls-file", str(urls),
                "--sources", "2", "--fixture", "/missing-fixture"], capture_output=True, text=True, timeout=3)
            self.assertEqual(result.returncode, 2)
            self.assertIn("distinct", result.stderr)


class GbPacketHealth(unittest.TestCase):
    def test_pes_split_across_rtp_packets(self):
        sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "bench"))
        from gb_sender_fanout import Sender, State
        state = State(argparse.Namespace(payload_type=96, video_only=True, viewers=1))
        state.expected[1] = Sender("sender", 1, time.monotonic(), "source")
        payload = b"\x00\x00\x01\xe0\x00\x0d\x80\x80\x05\x21\x00\x01\x00\x01frame"
        for sequence, part in enumerate((payload[:5], payload[5:13], payload[13:]), 1):
            packet = bytes((0x80, 96)) + sequence.to_bytes(2, "big") + bytes(4) + (1).to_bytes(4, "big") + part
            state.packet(packet, len(packet))
            self.assertEqual(state.video_by_ssrc.get(1, 0), int(sequence == 3))
        self.assertEqual(state.first_seen, {1})
        self.assertFalse(state.errors)

    def test_pes_pattern_inside_audio_is_not_header(self):
        sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "bench"))
        from gb_sender_fanout import Sender, State
        state = State(argparse.Namespace(payload_type=96, video_only=True, viewers=1))
        state.expected[1] = Sender("sender", 1, time.monotonic(), "source")
        def pes(stream, low, data):
            body = bytes((0x80, 0x80, 5, 0x21, 0, 1, 0, low)) + data
            return b"\x00\x00\x01" + bytes((stream,)) + len(body).to_bytes(2, "big") + body
        for sequence, low in enumerate((101, 51), 1):
            payload = pes(0xc0, 101, pes(0xed, low, b"audio-payload"))
            packet = bytes((0x80, 96)) + sequence.to_bytes(2, "big") + bytes(4) + (1).to_bytes(4, "big") + payload
            state.packet(packet, len(packet))
        self.assertFalse(state.errors)
        self.assertEqual(state.audio_by_ssrc[1], 2)
        self.assertEqual(state.video_by_ssrc.get(1, 0), 0)

    def test_reordered_rtp_is_not_healthy(self):
        sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "bench"))
        from gb_sender_fanout import Sender, State
        state = State(argparse.Namespace(payload_type=96, video_only=True, viewers=1))
        state.expected[1] = Sender("sender", 1, time.monotonic(), "source")
        for sequence in (1, 2, 1):
            packet = bytes((0x80, 96)) + sequence.to_bytes(2, "big") + bytes(4) + (1).to_bytes(4, "big") + b"\x00\x00\x01\xe0x"
            state.packet(packet, len(packet))
        self.assertGreater(state.sequence_gaps.get(1, 0), 0)
        self.assertTrue(state.errors)


class HlsHealth(unittest.TestCase):
    def run_hls(self, freeze=False):
        started = time.monotonic()
        claimed = set()
        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"
            def do_GET(self):
                if self.path.startswith("/entry/"):
                    if self.path in claimed:
                        self.send_response(403)
                        body = b""
                    else:
                        claimed.add(self.path)
                        self.send_response(307)
                        self.send_header("Location", self.path.replace("/entry/", "/session/") + "/index.m3u8")
                        body = b""
                elif self.path.endswith("m3u8"):
                    self.send_response(200)
                    elapsed = time.monotonic() - started
                    sequence = int(min(elapsed, 1) * 5) if freeze else int(elapsed * 5)
                    first = max(0, sequence - 10)
                    body = (f"#EXTM3U\n#EXT-X-TARGETDURATION:1\n#EXT-X-MEDIA-SEQUENCE:{first}\n" +
                            "".join(f"#EXTINF:0.2,\n{i}.ts\n" for i in range(first, sequence + 1))).encode()
                else:
                    self.send_response(200)
                    body = b"transport-stream" * 100
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                try:
                    self.wfile.write(body)
                except (BrokenPipeError, ConnectionResetError):
                    pass
            def log_message(self, *_):
                pass
        server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever)
        thread.start()
        try:
            with tempfile.TemporaryDirectory() as directory:
                urls = pathlib.Path(directory) / "urls"
                urls.write_text(f"http://127.0.0.1:{server.server_port}/entry/a\nhttp://127.0.0.1:{server.server_port}/entry/b\n")
                result = subprocess.run(["python3", str(pathlib.Path(__file__).resolve().parents[1] / "bench/hls_client.py"),
                    "--hls-urls-file", str(urls), "--viewers", "2", "--duration", "3", "--warmup", "0",
                    "--progress-interval", "1", "--ramp-per-second", "100", "--persistent"],
                    capture_output=True, text=True, timeout=7)
                return result
        finally:
            server.shutdown()
            server.server_close()
            thread.join()

    def test_independent_sessions_progress(self):
        result = self.run_hls()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("ready=2 progressing=2 failed=0", result.stdout)

    def test_growing_then_frozen_playlist_fails(self):
        result = self.run_hls(freeze=True)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("non_progressing_media", result.stdout)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=pathlib.Path, required=True)
    args, remaining = parser.parse_known_args()
    BUILD = args.build_dir.resolve()
    unittest.main(argv=[__file__, *remaining])
