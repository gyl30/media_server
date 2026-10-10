#!/usr/bin/env python3
import signal
import json
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "bench"))
from fanout_support import stop_process, wait_for_listener
from signaling_stub import SignalingStub

IDLE_TIMEOUT_SECONDS = 20


def main():
    server_bin, ffmpeg = sys.argv[1:3]
    held = [socket.socket() for _ in range(3)]
    for connection in held:
        connection.bind(("127.0.0.1", 0))
    ports = [connection.getsockname()[1] for connection in held]
    for connection in held:
        connection.close()
    publish = [ffmpeg, "-hide_banner", "-loglevel", "error", "-re", "-f", "lavfi", "-i", "testsrc2=size=320x240:rate=20",
               "-c:v", "libx264", "-preset", "ultrafast", "-g", "20", "-an", "-f", "flv"]
    with SignalingStub() as signaling, tempfile.TemporaryDirectory(prefix="media-idle-") as temporary:
        log_path = Path(temporary) / "server.log"
        with log_path.open("w") as server_log:
            server = subprocess.Popen([server_bin, "--threads", "2", "--rtmp-port", str(ports[0]), "--rtsp-port", str(ports[1]),
                                       "--http-port", str(ports[2]), "--signaling-url", signaling.url], stdout=server_log, stderr=subprocess.STDOUT)
            frozen = None
            replacement = None
            try:
                wait_for_listener("127.0.0.1", ports[0])
                stream_id = signaling.issue("publish")
                frozen = subprocess.Popen([*publish, f"rtmp://127.0.0.1:{ports[0]}/live/{stream_id}"],
                                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                time.sleep(3)
                # 冻结进程后 TCP 仍然存活，只能依靠服务端空闲超时释放输入登记。
                frozen.send_signal(signal.SIGSTOP)
                time.sleep(IDLE_TIMEOUT_SECONDS + 3)
                with urllib.request.urlopen(f"http://127.0.0.1:{ports[2]}/receivers", timeout=3) as response:
                    assert all(entry["stream_id"] != stream_id for entry in json.load(response)["receivers"]), "idle input remains registered"
                next_id = signaling.issue("publish")
                replacement = subprocess.Popen([*publish, f"rtmp://127.0.0.1:{ports[0]}/live/{next_id}"],
                                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                time.sleep(4)
                text = log_path.read_text()
                if replacement.poll() is not None or "duplicate stream" in text or text.count("rtmp publish tracks ready") < 2:
                    print("new publisher generation failed after idle input cleanup")
                    print(text)
                    return 1
                print("idle publisher released receiver; fresh one-use token established new generation")
                return 0
            finally:
                if frozen is not None:
                    frozen.send_signal(signal.SIGCONT)
                stop_process(replacement)
                stop_process(frozen)
                stop_process(server)


if __name__ == "__main__":
    sys.exit(main())
