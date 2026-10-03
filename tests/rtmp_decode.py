#!/usr/bin/env python3
import socket
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "bench"))
from fanout_support import stop_process, wait_for_listener, wait_for_stream


def main():
    server_bin, ffmpeg = sys.argv[1:3]
    video_codec = sys.argv[3] if len(sys.argv) > 3 else "h264"
    with tempfile.TemporaryDirectory(prefix="media-rtmp-decode-") as temporary:
        output = Path(temporary)
        fixture = output / "fixture.flv"
        command = [ffmpeg, "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i", "testsrc2=size=320x240:rate=20",
                        "-f", "lavfi", "-i", "sine=frequency=1000:sample_rate=44100", "-t", "3", "-c:v", "libx264",
                        "-preset", "ultrafast", "-tune", "zerolatency", "-g", "20", "-c:a", "aac", "-ac", "2"]
        if video_codec == "h265":
            command[command.index("libx264")] = "libx265"
            command += ["-x265-params", "pools=none:frame-threads=1:bframes=0:log-level=error"]
        subprocess.run([*command, str(fixture)], check=True, timeout=15)
        held = [socket.socket() for _ in range(3)]
        for connection in held:
            connection.bind(("127.0.0.1", 0))
        ports = [connection.getsockname()[1] for connection in held]
        for connection in held:
            connection.close()
        with (output / "server.log").open("w") as server_log, (output / "publish.log").open("w") as publish_log:
            server = subprocess.Popen([server_bin, "--threads", "2", "--rtmp-port", str(ports[0]), "--rtsp-port", str(ports[1]),
                                       "--http-port", str(ports[2]), "--bind-address", "127.0.0.1", "--webrtc-address", "127.0.0.1"],
                                      stdout=server_log, stderr=subprocess.STDOUT)
            publisher = None
            try:
                wait_for_listener("127.0.0.1", ports[0])
                publisher = subprocess.Popen([ffmpeg, "-hide_banner", "-loglevel", "error", "-stream_loop", "-1", "-re", "-i", str(fixture),
                                              "-c", "copy", "-f", "flv", f"rtmp://127.0.0.1:{ports[0]}/verify/decode"],
                                             stdout=publish_log, stderr=subprocess.STDOUT)
                wait_for_stream("127.0.0.1", ports[2], "verify/decode")
                for attempt in range(3):
                    decoded = subprocess.run([ffmpeg, "-hide_banner", "-loglevel", "info", "-rw_timeout", "2000000",
                                              "-analyzeduration", "500000", "-probesize", "500000",
                                              "-i", f"rtmp://127.0.0.1:{ports[0]}/verify/decode", "-map", "0:v:0", "-map", "0:a:0",
                                              "-t", "1", "-f", "null", "-"], capture_output=True, text=True, timeout=10)
                    assert decoded.returncode == 0, decoded.stderr
                    expected_video = "hevc" if video_codec == "h265" else "h264"
                    assert "Video: " + expected_video in decoded.stderr and "Audio: aac" in decoded.stderr and "time=00:00:01" in decoded.stderr, decoded.stderr
                    print(f"independent FFmpeg RTMP {video_codec}/AAC decode {attempt + 1}/3: PASS", flush=True)
            finally:
                stop_process(publisher)
                stop_process(server)
                log = (output / "server.log").read_text()
                assert server.returncode == 0 and "ERROR: AddressSanitizer" not in log and "runtime error:" not in log, log


if __name__ == "__main__":
    main()
