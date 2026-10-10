#!/usr/bin/env python3
import http.client
import json
import secrets
import socket
import subprocess
import sys
import tempfile
import threading
import time
import uuid
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "bench"))
from fanout_support import stop_process, wait_for_listener
from signaling_stub import SignalingStub


def request(port, path, method="GET", body=None, content_type="application/json", media=False):
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=12)
    try:
        connection.request(method, path, body, {"Content-Type": content_type} if body is not None else {})
        response = connection.getresponse()
        return response.status, dict(response.getheaders()), response.read(128 if media else None)
    finally:
        connection.close()


def describe(port, stream_id, token, body=None):
    url = f"rtsp://127.0.0.1:{port}/{stream_id}/{token}" if body is None else f"rtsp://127.0.0.1:{port}/{stream_id}"
    method = "DESCRIBE" if body is None else "ANNOUNCE"
    extra = "Accept: application/sdp\r\n" if body is None else f"Content-Type: application/sdp\r\nContent-Length: {len(body)}\r\n"
    with socket.create_connection(("127.0.0.1", port), timeout=5) as connection:
        connection.sendall(f"{method} {url} RTSP/1.0\r\nCSeq: 1\r\n{extra}\r\n".encode() + (body or b""))
        received = bytearray()
        while b"\r\n\r\n" not in received:
            data = connection.recv(4096)
            if not data:
                return 0
            received.extend(data)
        return int(received.split(b" ", 2)[1])


def offer(direction):
    fingerprint = ":".join(["11"] * 32)
    return ("v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\na=group:BUNDLE 0\r\n"
            "m=video 9 UDP/TLS/RTP/SAVPF 96\r\nc=IN IP4 0.0.0.0\r\na=mid:0\r\n"
            f"a={direction}\r\na=rtcp-mux\r\na=setup:actpass\r\na=ice-ufrag:auth-test\r\n"
            "a=ice-pwd:auth-test-password-0123456789\r\n"
            f"a=fingerprint:sha-256 {fingerprint}\r\n"
            "a=extmap:1 urn:ietf:params:rtp-hdrext:sdes:mid\r\na=rtpmap:96 H264/90000\r\n"
            "a=fmtp:96 packetization-mode=1;profile-level-id=42e01f;level-asymmetry-allowed=1\r\n")


def decode(ffmpeg, url, protocol, accepted):
    command = [ffmpeg, "-hide_banner", "-loglevel", "error"]
    if protocol == "rtsp":
        command += ["-rtsp_transport", "tcp"]
    else:
        command += ["-rw_timeout", "3000000"]
    command += ["-analyzeduration", "100000", "-probesize", "100000", "-i", url,
                "-map", "0:v:0", "-frames:v", "3", "-an", "-f", "framemd5", "-"]
    result = subprocess.run(command, capture_output=True, text=True, timeout=20)
    if accepted:
        frames = [line for line in result.stdout.splitlines() if line and not line.startswith("#")]
        assert result.returncode == 0 and len(frames) == 3, (url, result.stdout, result.stderr)
    else:
        assert result.returncode != 0, ("unexpected playback", url, result.stdout)


def receiver_ids(port):
    status, _, body = request(port, "/receivers")
    assert status == 200, body
    return {entry["stream_id"] for entry in json.loads(body)["receivers"]}


def wait_receiver(port, stream_id, present):
    deadline = time.monotonic() + 5
    while (stream_id in receiver_ids(port)) != present:
        assert time.monotonic() < deadline, ("receiver registration mismatch", stream_id, present)
        time.sleep(0.05)


def main():
    server_bin, ffmpeg = sys.argv[1:3]
    held = [socket.socket() for _ in range(3)]
    for connection in held:
        connection.bind(("127.0.0.1", 0))
    rtmp, rtsp, http = [connection.getsockname()[1] for connection in held]
    for connection in held:
        connection.close()
    with SignalingStub() as signaling, tempfile.TemporaryDirectory(prefix="media-stream-auth-") as temporary:
        output = Path(temporary)
        fixture = output / "source.flv"
        subprocess.run([ffmpeg, "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i", "testsrc2=size=160x120:rate=10",
                        "-f", "lavfi", "-i", "sine=sample_rate=48000", "-t", "2", "-c:v", "libx264", "-preset", "ultrafast",
                        "-tune", "zerolatency", "-g", "10", "-c:a", "aac", "-ac", "2", str(fixture)], check=True, timeout=15)
        publishers = []
        with (output / "server.log").open("w") as log:
            server = subprocess.Popen([server_bin, "--threads", "2", "--bind-address", "127.0.0.1", "--webrtc-address", "127.0.0.1",
                                       "--rtmp-port", str(rtmp), "--rtsp-port", str(rtsp), "--http-port", str(http),
                                       "--signaling-url", signaling.url], stdout=log, stderr=subprocess.STDOUT)
            try:
                wait_for_listener("127.0.0.1", http)
                source_ids = []
                for protocol in ("rtmp", "rtsp"):
                    stream_id = signaling.issue("publish")
                    source_ids.append(stream_id)
                    if protocol == "rtsp":
                        before = signaling.count()
                        assert describe(rtsp, stream_id, "", b"invalid SDP") != 200
                        assert signaling.count() == before and signaling.pending(stream_id), "invalid ANNOUNCE consumed token"
                        sdp = (b"v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\ns=auth-test\r\nc=IN IP4 127.0.0.1\r\nt=0 0\r\n"
                               b"m=video 0 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\n"
                               b"a=fmtp:96 packetization-mode=1;sprop-parameter-sets=Z0IAH5WoFAFuQA==,aM4G4g==\r\n"
                               b"a=control:trackID=1\r\n")
                        assert describe(rtsp, "invalid-stream-id", "", sdp) != 200
                        assert signaling.count() == before, "invalid ANNOUNCE URI requested authorization"
                        receivers_before = receiver_ids(http)
                        assert describe(rtsp, secrets.token_hex(32), "", sdp) != 200
                        assert signaling.count() == before + 1 and signaling.pending(stream_id), "unauthorized ANNOUNCE accepted"
                        assert receiver_ids(http) == receivers_before, "rejected ANNOUNCE changed receiver registrations"
                    url = f"rtmp://127.0.0.1:{rtmp}/live/{stream_id}" if protocol == "rtmp" else f"rtsp://127.0.0.1:{rtsp}/{stream_id}"
                    command = [ffmpeg, "-hide_banner", "-loglevel", "error", "-stream_loop", "-1", "-re", "-i", str(fixture), "-c", "copy"]
                    command += ["-f", "flv"] if protocol == "rtmp" else ["-rtsp_transport", "tcp", "-f", "rtsp"]
                    publisher = subprocess.Popen([*command, url], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                    publishers.append(publisher)
                    wait_receiver(http, stream_id, True)
                    assert not signaling.pending(stream_id), "publish token was not consumed"
                    # A successful preflight request proves fixed tracks exist before any auth assertions.
                    deadline = time.monotonic() + 10
                    while True:
                        token = signaling.issue("play", stream_id)
                        status, _, _ = request(http, f"/{stream_id}/{token}.flv", media=True)
                        if status == 200:
                            break
                        assert status == 404 and publisher.poll() is None and time.monotonic() < deadline, status
                        time.sleep(0.05)
                    print(f"{protocol} authenticated publish + fixed tracks: PASS", flush=True)

                source, alternate = source_ids
                for token in (secrets.token_hex(32), signaling.issue("play", source, ttl=0)):
                    status, headers, _ = request(http, f"/play/hls/{source}/{token}/index.m3u8")
                    assert status == 403 and "Location" not in headers, (status, headers)
                print("HLS invalid/expired authorization rejected: PASS", flush=True)
                hls_viewers = []
                for protocol in ("rtmp", "rtsp", "flv", "hls", "whep"):
                    token = signaling.issue("play", source)

                    def play(stream_id, accepted):
                        if protocol == "rtmp":
                            decode(ffmpeg, f"rtmp://127.0.0.1:{rtmp}/{stream_id}/{token}", protocol, accepted)
                        elif protocol == "rtsp":
                            if accepted:
                                decode(ffmpeg, f"rtsp://127.0.0.1:{rtsp}/{stream_id}/{token}", protocol, True)
                            else:
                                assert describe(rtsp, stream_id, token) != 200
                        elif protocol == "flv":
                            status, _, body = request(http, f"/{stream_id}/{token}.flv", media=True)
                            assert (status == 200) == accepted, (protocol, status)
                            if accepted:
                                assert body.startswith(b"FLV"), body
                                decode_token = signaling.issue("play", stream_id)
                                decode(ffmpeg, f"http://127.0.0.1:{http}/{stream_id}/{decode_token}.flv", protocol, True)
                                assert not signaling.pending(decode_token), "FLV decode token was not consumed"
                        elif protocol == "hls":
                            status, headers, _ = request(http, f"/play/hls/{stream_id}/{token}/index.m3u8")
                            assert (status == 307) == accepted, (protocol, status)
                            if accepted:
                                location = headers["Location"]
                                parts = location.split("/")
                                assert parts[:4] == ["", "play", "hls", "session"] and len(parts) == 6 and parts[5] == "index.m3u8", location
                                uuid.UUID(parts[4])
                                status, _, body = request(http, location)
                                assert status == 200 and body.startswith(b"#EXTM3U"), (status, body)
                                segments = [line for line in body.decode().splitlines() if line and not line.startswith("#")]
                                assert segments and all(line.endswith(".ts") and line[:-3].isdigit() for line in segments), segments
                                segment = segments[-1]
                                status, _, packet = request(http, location.rsplit("/", 1)[0] + "/" + segment)
                                assert status == 200 and packet, (status, packet)
                                assert request(http, location.rsplit("/", 1)[0] + "/18446744073709551615.ts")[0] == 404
                                second_token = signaling.issue("play", stream_id)
                                status, headers, _ = request(http, f"/play/hls/{stream_id}/{second_token}/index.m3u8")
                                assert status == 307 and not signaling.pending(second_token), (status, headers)
                                second = headers["Location"]
                                assert second != location, "viewers must have independent playback identities"
                                status, _, shared_packet = request(http, second.rsplit("/", 1)[0] + "/" + segment)
                                assert status == 200 and shared_packet == packet, "viewers did not share the existing segment"
                                decode(ffmpeg, f"http://127.0.0.1:{http}{second}", protocol, True)
                                hls_viewers.extend((location, second))
                            else:
                                assert status in (403, 404) and "Location" not in headers, (status, headers)
                        else:
                            status, headers, answer = request(http, f"/play/whep/{stream_id}/{token}", "POST", offer("recvonly"), "application/sdp")
                            assert (status == 201) == accepted, (protocol, status, answer)
                            if accepted:
                                assert b"H264/90000" in answer, answer
                                assert request(http, headers["Location"], "DELETE")[0] == 204

                    play(alternate, False)
                    assert signaling.pending(token), f"{protocol} wrong stream consumed token"
                    play(source, True)
                    assert not signaling.pending(token), f"{protocol} token not consumed"
                    play(source, False)
                    token = signaling.issue("play", str(uuid.uuid4()))
                    count = signaling.count()
                    play(str(uuid.uuid4()), False)
                    assert signaling.count() == count and signaling.pending(token), f"{protocol} missing source called verify"
                    print(f"{protocol} play valid/replay/wrong-ID/missing-source: PASS", flush=True)

                token = signaling.issue("play", source)
                count = signaling.count()
                with socket.create_connection(("127.0.0.1", rtsp), timeout=5) as connection:
                    connection.sendall((f"SETUP rtsp://127.0.0.1:{rtsp}/{source}/{token}/trackID=1 RTSP/1.0\r\n"
                                        "CSeq: 1\r\nTransport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n\r\n").encode())
                    assert not connection.recv(4096).startswith(b"RTSP/1.0 200"), "SETUP admitted without DESCRIBE"
                for path in (f"/{source}/{token}.flv", f"/play/hls/{source}/{token}/index.m3u8"):
                    assert request(http, path, "HEAD")[0] == 405
                    assert request(http, path + "?x=1")[0] != 200
                    assert request(http, path + "?")[0] != 200
                    assert request(http, path + "#fragment")[0] != 200
                    assert request(http, "/./" + path.lstrip("/"))[0] != 200
                    assert request(http, path.replace(token, "%61" + token[1:]))[0] != 200
                    assert request(http, path + "/extra")[0] != 200
                    assert request(http, path.replace(token, "A" + token[1:]))[0] != 200
                for path in ("/play/hls/session", "/play/hls/session/a/b/c"):
                    assert request(http, path)[0] not in (200, 307)
                assert request(http, f"/play/whep/{source}/{token}", "POST", "invalid SDP", "application/sdp")[0] == 400
                assert signaling.count() == count and signaling.pending(token), "local precheck consumed token"
                print("HEAD, malformed paths and invalid SDP do not verify: PASS", flush=True)

                whip_id = signaling.issue("publish")
                before = signaling.count()
                assert request(http, f"/publish/whip/{whip_id}", "POST", "invalid SDP", "application/sdp")[0] == 400
                assert signaling.count() == before and signaling.pending(whip_id), "WHIP invalid offer consumed token"
                status, headers, answer = request(http, f"/publish/whip/{whip_id}", "POST", offer("sendonly"), "application/sdp")
                assert status == 201 and b"H264/90000" in answer, (status, answer)
                wait_receiver(http, whip_id, True)
                assert not signaling.pending(whip_id)
                source_ids.append(whip_id)
                for stream_id in source_ids:
                    assert request(http, "/receivers/delete", "POST", json.dumps({"stream_id": stream_id}))[0] == 204
                    wait_receiver(http, stream_id, False)
                    assert request(http, "/receivers/delete", "POST", json.dumps({"stream_id": stream_id}))[0] == 404
                for publisher in publishers:
                    publisher.wait(timeout=5)
                for location in hls_viewers:
                    deadline = time.monotonic() + 5
                    while True:
                        status, _, body = request(http, location)
                        assert status == 200, (status, body)
                        if b"#EXT-X-ENDLIST" in body:
                            break
                        assert time.monotonic() < deadline, "HLS source end was not delivered"
                        time.sleep(0.05)
                    segment = [line for line in body.decode().splitlines() if line and not line.startswith("#")][-1]
                    assert request(http, location.rsplit("/", 1)[0] + "/" + segment)[0] == 200
                print("HLS shared viewers decode + retained playlist/segment after source end: PASS", flush=True)
                assert request(http, f"/publish/whip/{whip_id}", "POST", offer("sendonly"), "application/sdp")[0] != 201
                for protocol, stream_id in zip(("rtmp", "rtsp"), source_ids):
                    command = [ffmpeg, "-hide_banner", "-loglevel", "error", "-re", "-i", str(fixture), "-c", "copy", "-t", "1"]
                    command += (["-f", "flv", f"rtmp://127.0.0.1:{rtmp}/live/{stream_id}"] if protocol == "rtmp"
                                else ["-rtsp_transport", "tcp", "-f", "rtsp", f"rtsp://127.0.0.1:{rtsp}/{stream_id}"])
                    result = subprocess.run(command, capture_output=True, timeout=10)
                    assert result.returncode != 0 and stream_id not in receiver_ids(http), (protocol, result.stderr)
                for protocol in ("rtmp", "rtsp", "whip"):
                    wrong_operation = signaling.issue("play", source)
                    if protocol == "whip":
                        assert request(http, f"/publish/whip/{wrong_operation}", "POST", offer("sendonly"), "application/sdp")[0] != 201
                    else:
                        command = [ffmpeg, "-hide_banner", "-loglevel", "error", "-i", str(fixture), "-c", "copy", "-t", "1"]
                        command += (["-f", "flv", f"rtmp://127.0.0.1:{rtmp}/live/{wrong_operation}"] if protocol == "rtmp"
                                    else ["-rtsp_transport", "tcp", "-f", "rtsp", f"rtsp://127.0.0.1:{rtsp}/{wrong_operation}"])
                        result = subprocess.run(command, capture_output=True, timeout=10)
                        assert result.returncode != 0, (protocol, result.stderr)
                    assert signaling.pending(wrong_operation), f"{protocol} wrong operation consumed token"
                print("three publish entrances + unified delete + replay rejection: PASS", flush=True)
            except Exception:
                print((output / "server.log").read_text(), file=sys.stderr)
                raise
            finally:
                for publisher in publishers:
                    stop_process(publisher)
                stop_process(server)
            text = (output / "server.log").read_text()
            assert server.returncode == 0 and "ERROR: AddressSanitizer" not in text and "runtime error:" not in text, text

        for protocol in ("rtmp", "rtsp"):
            signaling.response_gate = threading.Event()
            signaling.verify_started.clear()
            signaling.verify_finished.clear()
            stream_id = signaling.issue("publish")
            with (output / f"verify-shutdown-{protocol}.log").open("w") as log:
                server = subprocess.Popen([server_bin, "--threads", "1", "--bind-address", "127.0.0.1",
                                           "--rtmp-port", str(rtmp), "--rtsp-port", str(rtsp), "--http-port", str(http),
                                           "--signaling-url", signaling.url], stdout=log, stderr=subprocess.STDOUT)
                publisher = None
                try:
                    wait_for_listener("127.0.0.1", http)
                    command = [ffmpeg, "-hide_banner", "-loglevel", "error", "-re", "-i", str(fixture), "-c", "copy"]
                    command += (["-f", "flv", f"rtmp://127.0.0.1:{rtmp}/live/{stream_id}"] if protocol == "rtmp"
                                else ["-rtsp_transport", "tcp", "-f", "rtsp", f"rtsp://127.0.0.1:{rtsp}/{stream_id}"])
                    publisher = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                    assert signaling.verify_started.wait(timeout=5), f"{protocol} did not reach verification"
                    assert stream_id not in receiver_ids(http), "publish registered before verification response"
                    server.terminate()
                    server.wait(timeout=5)
                    assert server.returncode == 0, f"{protocol} shutdown during verify exited {server.returncode}"
                    publisher.wait(timeout=5)
                    assert publisher.returncode != 0, "publish activated after worker shutdown"
                finally:
                    signaling.response_gate.set()
                    stop_process(publisher)
                    stop_process(server)
                assert signaling.verify_finished.wait(timeout=3), "verification handler retained after shutdown"
            text = (output / f"verify-shutdown-{protocol}.log").read_text()
            assert "ERROR: AddressSanitizer" not in text and "runtime error:" not in text, text
            print(f"{protocol} worker shutdown during pending verification: PASS", flush=True)
        signaling.response_gate = None


if __name__ == "__main__":
    main()
