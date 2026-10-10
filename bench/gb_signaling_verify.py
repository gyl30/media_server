#!/usr/bin/env python3
"""Verify GB device management and one-time WHEP using SIP/RTP and Chrome."""

import argparse
import concurrent.futures
import json
import os
import re
import signal
import subprocess
import time
from pathlib import Path
from urllib.parse import urlsplit

from fanout_support import benchmark_head, stop_process, wait_for_listener, wait_for_stream
from lifecycle_verify import request


def eventually(check, timeout=15):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if check():
            return
        time.sleep(0.1)
    raise AssertionError("condition did not become true")


def main():
    from playwright.sync_api import sync_playwright

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--media", type=Path, required=True)
    parser.add_argument("--signaling", type=Path, required=True)
    parser.add_argument("--simulator", type=Path, required=True)
    parser.add_argument("--media-file", type=Path, help="Annex-B H264 fixture with AUD")
    parser.add_argument("--audio-file", type=Path, help="raw PCMA fixture, 8000 Hz mono")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--ffmpeg", default="/home/gyl/bin/ffmpeg")
    parser.add_argument("--browser", default="/usr/bin/google-chrome")
    parser.add_argument("--port-base", type=int, default=43120)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    base = args.port_base
    media_port, api_port, sip_port = base + 2, base + 3, base + 4
    device = "34020000001320000001"
    channel = "34020000001320000002"
    device_path = f"/api/devices/{device}"
    play_path = f"{device_path}/channels/{channel}/play"
    fixture = args.media_file or args.output / "fixture.h264"
    processes, commands = [], []
    result = {"head": benchmark_head(), "status": "FAIL", "checks": [], "viewer_stats": []}

    def launch(label, command):
        command = list(map(str, command))
        path = args.output / f"{label}.log"
        with path.open("w") as log:
            process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
        process.log_path = path
        processes.append(process)
        commands.append({"label": label, "command": command})
        return process

    def api(method, path, body=None, expected=200):
        status, headers, data = request(api_port, method, path, body)
        assert status == expected, (method, path, status, data.decode(errors="replace"))
        return json.loads(data) if data else None

    def consume(ticket, expected=404):
        path = urlsplit(ticket["whep_url"]).path
        status = request(api_port, "POST", path, "invalid SDP", "application/sdp")[0]
        assert status == expected, (path, status)

    def channels():
        return api("GET", device_path + "/channels")["channels"]

    def online():
        return api("GET", device_path)["online"] and len(channels()) == 1

    def mark(name):
        result["checks"].append(name)
        print(f"PASS {name}", flush=True)

    def start_media(label):
        process = launch(label, [args.media, "--bind-address", "127.0.0.1", "--webrtc-address", "127.0.0.1",
            "--rtmp-port", base, "--rtsp-port", base + 1, "--http-port", media_port, "--threads", "6"])
        wait_for_listener("127.0.0.1", media_port)
        return process

    def start_signaling(label):
        process = launch(label, [args.signaling, "--database", args.output / "devices.db",
            "--sip-listen", f"127.0.0.1:{sip_port}", "--sip-advertise", f"127.0.0.1:{sip_port}",
            "--http-listen", f"127.0.0.1:{api_port}", "--media-control-url", f"http://127.0.0.1:{media_port}",
            "--media-http-port", media_port, "--heartbeat-timeout", "20s"])
        wait_for_listener("127.0.0.1", api_port)
        return process

    def start_simulator(label):
        command = [args.simulator, "--platform-sip", f"127.0.0.1:{sip_port}", "--listen", f"127.0.0.1:{base+5}",
            "--media-file", fixture, "--duration", "5m", "--register-expires", "12s", "--heartbeat", "1s",
            "--control-workers", "2", "--media-workers", "1"]
        if args.audio_file:
            command += ["--audio-file", args.audio_file]
        return launch(label, command)

    def counters(sim):
        lines = [line for line in sim.log_path.read_text().splitlines() if "simulator summary" in line]
        assert lines, sim.log_path.read_text()
        return {key: int(value) for key, value in re.findall(r"(\w+)=(\d+)(?=\s|$)", lines[-1])}

    def ended(live_id, resources):
        for resource in resources:
            endpoint = urlsplit(resource)
            eventually(lambda: request(endpoint.port, "GET", endpoint.path)[0] == 404)
        assert request(media_port, "POST", "/receivers/delete", {"stream_id": live_id})[0] == 404

    started = time.monotonic()
    try:
        if not args.media_file:
            subprocess.run([args.ffmpeg, "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i", "testsrc2=size=320x240:rate=25",
                "-t", "5", "-c:v", "libx264", "-preset", "ultrafast", "-tune", "zerolatency", "-pix_fmt", "yuv420p",
                "-g", "25", "-bf", "0", "-bsf:v", "h264_metadata=aud=insert", "-an", "-f", "h264", str(fixture)], check=True)
        media = start_media("media-1")
        signaling = start_signaling("signaling-1")
        api("POST", "/api/devices", {"device_id": device, "name": "E2E camera"}, 201)
        assert not api("GET", device_path)["online"]
        assert len(api("GET", "/api/devices")["devices"]) == 1
        mark("device persisted and visible while offline")
        simulator = start_simulator("simulator-1")
        eventually(online)
        mark("REGISTER allowlist and automatic Catalog")
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            tickets = list(pool.map(lambda _: api("POST", play_path, expected=201), range(8)))
        live_id = tickets[0]["live_id"]
        assert len({ticket["live_id"] for ticket in tickets}) == 1
        assert len({ticket["play_id"] for ticket in tickets}) == 8
        wait_for_stream("127.0.0.1", media_port, live_id)
        mark("eight concurrent play requests share one live")

        if args.audio_file:
            command = [args.ffmpeg, "-hide_banner", "-loglevel", "info", "-rtsp_transport", "tcp",
                "-i", f"rtsp://127.0.0.1:{base+1}/{live_id}", "-map", "0:v:0", "-map", "0:a:0",
                "-t", "4", "-c:v", "rawvideo", "-threads:v", "1", "-c:a", "pcm_s16le", "-f", "framecrc", "-"]
            decoded = subprocess.run(command, capture_output=True, text=True, timeout=30)
            commands.append({"label": "rtsp-decode", "command": command})
            (args.output / "rtsp-decode.log").write_text(decoded.stderr)
            (args.output / "rtsp-decode.framecrc").write_text(decoded.stdout)
            assert decoded.returncode == 0, decoded.stderr
            assert "Audio: pcm_alaw, 8000 Hz, mono" in decoded.stderr, decoded.stderr
            packets = [[], []]
            time_bases = {}
            for line in decoded.stdout.splitlines():
                match = re.match(r"#tb (\d+): (\d+)/(\d+)", line)
                if match:
                    index, numerator, denominator = map(int, match.groups())
                    time_bases[index] = numerator / denominator
                elif line and not line.startswith("#"):
                    index, dts, pts, duration, size = map(int, line.split(",")[:5])
                    packets[index].append({"pts": pts, "duration": duration, "bytes": size})
            assert all(len(track) > 1 for track in packets), packets
            for track in packets:
                assert all(second["pts"] > first["pts"] for first, second in zip(track, track[1:])), track
            result["rtsp_decode"] = {"returncode": decoded.returncode, "video_frames": len(packets[0]),
                "audio_packets": len(packets[1]), "audio_samples": sum(item["bytes"] // 2 for item in packets[1]),
                "audio_seconds": sum(item["duration"] for item in packets[1]) * time_bases[1], "pts_monotonic": True}
            assert result["rtsp_decode"]["video_frames"] >= 50 and result["rtsp_decode"]["audio_seconds"] >= 3.5, result["rtsp_decode"]
            mark("RTSP H264 and PCMA decode with monotonic video/audio PTS")

        with sync_playwright() as playwright:
            browser = playwright.chromium.launch(executable_path=args.browser, headless=True,
                args=["--autoplay-policy=no-user-gesture-required"])
            result["browser_version"] = browser.version
            try:
                page = browser.new_page()
                page.goto(f"http://127.0.0.1:{api_port}/")
                page.evaluate("window.peers = []; window.resources = []; window.videos = []")

                def viewer(ticket, concurrent=False):
                    response = page.evaluate("""async ({url, concurrent, mediaPort, audio}) => {
                        const peer = new RTCPeerConnection();
                        window.peers.push(peer);
                        const video = document.createElement('video');
                        video.autoplay = video.muted = video.playsInline = true;
                        document.body.append(video);
                        window.videos.push(video);
                        peer.addTransceiver('video', {direction: 'recvonly'});
                        if (audio) peer.addTransceiver('audio', {direction: 'recvonly'});
                        const stream = new MediaStream(); video.srcObject = stream;
                        peer.ontrack = event => {stream.addTrack(event.track); void video.play();};
                        await peer.setLocalDescription(await peer.createOffer());
                        if (peer.iceGatheringState !== 'complete') await new Promise((resolve, reject) => {
                            const timer = setTimeout(() => reject(new Error('ICE gathering timeout')), 10000);
                            peer.onicegatheringstatechange = () => {
                                if (peer.iceGatheringState === 'complete') {clearTimeout(timer); resolve();}
                            };
                        });
                        const send = () => fetch(url, {method: 'POST', headers: {'Content-Type':'application/sdp'}, body:peer.localDescription.sdp});
                        const responses = concurrent ? await Promise.all([send(), send()]) : [await send()];
                        const statuses = responses.map(item => item.status).sort();
                        if (JSON.stringify(statuses) !== JSON.stringify(concurrent ? [201,404] : [201])) throw new Error(`POST statuses ${statuses}`);
                        const response = responses.find(item => item.status === 201);
                        const resource = response.headers.get('Location');
                        if (new URL(resource).port !== String(mediaPort)) throw new Error('Location does not point directly to media');
                        if (!response.headers.get('Content-Type').startsWith('application/sdp')) throw new Error('invalid answer type');
                        if (!response.headers.get('Access-Control-Expose-Headers').includes('Location')) throw new Error('Location not exposed');
                        window.resources.push(resource);
                        await peer.setRemoteDescription({type:'answer',sdp:await response.text()});
                        return {statuses, resource};
                    }""", {"url": ticket["whep_url"], "concurrent": concurrent, "mediaPort": media_port, "audio": bool(args.audio_file)})
                    eventually(lambda: page.evaluate("""async audio => {
                        const peer = peers[peers.length-1];
                        const stats = [...(await peer.getStats()).values()];
                        return peer.connectionState === 'connected' && stats.some(item => item.type === 'inbound-rtp' && item.framesDecoded > 0)
                            && (!audio || stats.some(item => item.type === 'inbound-rtp' && item.kind === 'audio' && item.totalSamplesReceived > 0));
                    }""", bool(args.audio_file)))
                    return response["resource"]

                def stats():
                    return page.evaluate("""async () => Promise.all(peers.map(async peer => {
                        const values = [...(await peer.getStats()).values()];
                        const video = values.find(item => item.type === 'inbound-rtp' && item.kind === 'video');
                        const codec = values.find(item => item.id === video.codecId);
                        const audio = values.find(item => item.type === 'inbound-rtp' && item.kind === 'audio');
                        const audioCodec = audio && values.find(item => item.id === audio.codecId);
                        return {framesDecoded:video.framesDecoded,bytesReceived:video.bytesReceived,codec:codec.mimeType,
                            audio:audio && {samples:audio.totalSamplesReceived,bytesReceived:audio.bytesReceived,
                                codec:audioCodec?.mimeType,clockRate:audioCodec?.clockRate},
                            connected:peer.connectionState === 'connected',dtls:values.some(item => item.dtlsState === 'connected')};
                    }))""")

                resources = []
                result["viewer_stats"] = []
                for index in range(3):
                    resources.append(viewer(tickets[index], index == 0))
                    consume(tickets[index])
                    before = stats()
                    page.wait_for_timeout(2000)
                    after = stats()
                    assert all(b["framesDecoded"] > a["framesDecoded"] and b["bytesReceived"] > a["bytesReceived"]
                        and b["codec"] == "video/H264" and b["connected"] and b["dtls"] for a, b in zip(before, after))
                    if args.audio_file:
                        assert all(b["audio"]["samples"] > a["audio"]["samples"]
                            and b["audio"]["bytesReceived"] > a["audio"]["bytesReceived"]
                            and b["audio"]["codec"].lower() == "audio/pcma" and b["audio"]["clockRate"] == 8000
                            for a, b in zip(before, after))
                    assert len(set(resources)) == len(resources)
                    result["viewer_stats"].append({"viewers": index+1, "before": before, "after": after})
                count = counters(simulator)
                assert count["invite"] == 1 and count["ack"] == 1 and count["live_active"] == 1 and count["rtp_packets"] > 0, count
                result["single_upstream_counters"] = count
                mark("three Chrome decoders progress from one INVITE/RTP source; concurrent ticket consume 201/404")
                if args.audio_file:
                    page.evaluate("peers.shift().close(); videos.shift().remove(); resources.shift()")
                    endpoint = urlsplit(resources.pop(0))
                    assert request(endpoint.port, "DELETE", endpoint.path)[0] in (204, 404)
                    assert request(endpoint.port, "GET", endpoint.path)[0] == 404
                    before = stats()
                    page.wait_for_timeout(2000)
                    after = stats()
                    assert all(b["framesDecoded"] > a["framesDecoded"] and b["audio"]["samples"] > a["audio"]["samples"]
                        for a, b in zip(before, after))
                    result["viewer_close"] = {"remaining": 2, "before": before, "after": after}
                    page.evaluate("peers.forEach(peer => peer.close()); videos.forEach(video => video.remove()); peers=[]; videos=[]; resources=[]")
                    for resource in resources:
                        endpoint = urlsplit(resource)
                        assert request(endpoint.port, "DELETE", endpoint.path)[0] in (204, 404)
                        assert request(endpoint.port, "GET", endpoint.path)[0] == 404
                    count = counters(simulator)
                    assert count["invite"] == count["ack"] == count["live_active"] == 1 and count["bye"] == 0, count
                    assert channels()[0]["live"]["live_id"] == live_id
                    result["all_viewers_closed"] = count
                    resources = []
                    mark("closing one PCMA viewer preserves others; closing all preserves the upstream live")
                page.evaluate("""async target => {
                    const {WHEPPreview} = await import('/whep.js');
                    const video = document.createElement('video');
                    video.muted = true; video.autoplay = true;
                    document.body.append(video);
                    window.preview = new WHEPPreview(video, update => {window.previewState = update.state;});
                    await preview.start(target, 'E2E camera');
                }""", {"device_id": device, "channel_id": channel})
                eventually(lambda: page.evaluate("""async () => preview.current && previewState === 'streaming' &&
                    [...(await preview.current.peer.getStats()).values()].some(item => item.framesDecoded > 0)"""))
                page.evaluate("async () => preview.closeViewer()")
                assert counters(simulator)["invite"] == 1
                mark("embedded Web GB player uses play ticket and leaves shared upstream running after viewer stop")
                expired = api("POST", play_path, expected=201)
                page.wait_for_timeout(31000)
                consume(expired)
                assert channels()[0]["live"]["live_id"] == live_id
                count = counters(simulator)
                assert count["invite"] == 1 and count["catalog"] == 1 and count["register_refresh"] > 0, count
                fresh = api("POST", play_path, expected=201)
                resources.append(viewer(fresh))
                mark("30 second expiration keeps live; refresh does not repeat Catalog; fresh ticket plays")
                pending = api("POST", play_path, expected=201)
                api("DELETE", f"/api/lives/{live_id}", expected=204)
                consume(pending)
                ended(live_id, resources)
                api("DELETE", f"/api/lives/{live_id}", expected=404)
                mark("live stop invalidates tickets and ends existing media viewers")
                page.evaluate("peers.forEach(peer => peer.close()); videos.forEach(video => video.remove()); peers=[]; videos=[]; resources=[]")

                live = api("POST", play_path, expected=201)
                wait_for_stream("127.0.0.1", media_port, live["live_id"])
                resource = viewer(live)
                pending = api("POST", play_path, expected=201)
                stop_process(simulator)
                assert simulator.returncode == 0, simulator.log_path.read_text()
                eventually(lambda: not api("GET", device_path)["online"])
                assert channels() == []
                consume(pending)
                ended(live["live_id"], [resource])
                mark("Expires:0 stops live and clears tickets/channels while retaining device")
                simulator = start_simulator("simulator-2")
                eventually(online)
                new_live = api("POST", play_path, expected=201)
                assert new_live["live_id"] != live["live_id"]
                wait_for_stream("127.0.0.1", media_port, new_live["live_id"])
                resource = viewer(new_live)
                pending = api("POST", play_path, expected=201)
                mark("re-registration synchronizes Catalog and starts a new generation")

                os.kill(simulator.pid, signal.SIGSTOP)
                stop_process(signaling)
                assert signaling.returncode == 0, signaling.log_path.read_text()
                signaling = start_signaling("signaling-2")
                assert not api("GET", device_path)["online"] and channels() == []
                consume(pending)
                api("DELETE", f"/api/lives/{new_live['live_id']}", expected=404)
                ended(new_live["live_id"], [resource])
                os.kill(simulator.pid, signal.SIGCONT)
                eventually(online)
                mark("signaling restart restores only device; REGISTER/Catalog recovers runtime")

                stop_process(media)
                api("POST", play_path, expected=502)
                assert "live" not in channels()[0]
                media = start_media("media-2")
                live = api("POST", play_path, expected=201)
                wait_for_stream("127.0.0.1", media_port, live["live_id"])
                mark("unavailable receiver creation leaves no live and permits a later play")
                stop_process(media)
                consume(live, 502)
                media = start_media("media-3")
                consume(live)
                api("DELETE", f"/api/lives/{live['live_id']}", expected=204)
                mark("proxy failure consumes ticket even after media recovers")

                page.evaluate("peers.forEach(peer => peer.close()); videos.forEach(video => video.remove()); peers=[]; videos=[]; resources=[]")
                live = api("POST", play_path, expected=201)
                wait_for_stream("127.0.0.1", media_port, live["live_id"])
                resource = viewer(live)
                pending = api("POST", play_path, expected=201)
                api("DELETE", device_path, expected=204)
                consume(pending)
                ended(live["live_id"], [resource])
                api("GET", device_path, expected=404)
                api("GET", device_path + "/channels", expected=404)
                assert api("GET", "/api/devices")["devices"] == []
                eventually(lambda: counters(simulator)["live_active"] == 0)
                result["delete_device_counters"] = counters(simulator)
                assert result["delete_device_counters"]["bye"] > 0
                mark("deleting playing device stops upstream/viewer, invalidates ticket and removes persistent device")
                stop_process(simulator)
                rejected = start_simulator("simulator-rejected")
                assert rejected.wait(timeout=10) != 0 and "REGISTER challenge status 403" in rejected.log_path.read_text()
                mark("deleted device REGISTER is rejected with 403")
                result["status"] = "PASS"
            finally:
                browser.close()
    except Exception as error:
        result["error"] = f"{type(error).__name__}: {error}"
        raise
    finally:
        for process in reversed(processes):
            if process.poll() is None:
                os.kill(process.pid, signal.SIGCONT)
                stop_process(process)
        result["duration_seconds"] = time.monotonic() - started
        result["process_returncodes"] = {process.log_path.name: process.returncode for process in processes}
        (args.output / "commands.json").write_text(json.dumps(commands, indent=2) + "\n")
        (args.output / "result.json").write_text(json.dumps(result, indent=2) + "\n")


if __name__ == "__main__":
    main()
