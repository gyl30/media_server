import argparse
import pathlib
import socketserver
import struct
import subprocess
import tempfile
import threading
import time
import unittest


class NativeCredentials(unittest.TestCase):
    def test_help_needs_no_credentials_or_service(self):
        for protocol in ("rtmp", "rtsp"):
            result = subprocess.run([str(BUILD / f"{protocol}_fanout"), "--help"], capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("--play-paths-file", result.stdout)

    def test_distinct_paths_reach_protocol_client(self):
        for protocol in ("rtmp", "rtsp"):
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
        for protocol in ("rtmp", "rtsp"):
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
    def run_client(self, tracks, duration=1, stall_after=None, warmup=0):
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
                                    packet = struct.pack("!BBHII", 0x80, 96 + channel, sequence & 65535, sequence * 3000, channel + 1)
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
                     *(["--video-only"] if not any(kind == "audio" for _, kind, _ in tracks) else [])],
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


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=pathlib.Path, required=True)
    args, remaining = parser.parse_known_args()
    BUILD = args.build_dir.resolve()
    unittest.main(argv=[__file__, *remaining])
