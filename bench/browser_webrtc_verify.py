#!/usr/bin/env python3
"""Verify public WHEP/WHIP with Chrome's real media encoder and decoder."""

import argparse
import contextlib
import json
import re
import subprocess
import time
from pathlib import Path

from playwright.sync_api import sync_playwright

from fanout_support import benchmark_head, stop_process, wait_for_stream
from lifecycle_verify import Run, request


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("direction", choices=("whep", "whip"))
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--ffmpeg", type=Path, default=Path("/home/gyl/bin/ffmpeg"))
    parser.add_argument("--browser", default="/usr/bin/google-chrome")
    parser.add_argument("--port-base", type=int, default=42930)
    parser.add_argument("--duration", type=int, default=30)
    parser.add_argument("--resume-video", action="store_true", help="WHIP: pause/resume the publisher after the player joins")
    parser.add_argument("--expected-video", choices=("h264", "h265"), default="h264")
    parser.add_argument("--expected-audio", choices=("opus", "pcma", "pcmu", "none"), default="opus")
    args = parser.parse_args()
    if args.duration < 1:
        parser.error("duration must be positive")
    if args.direction == "whip" and (args.expected_video != "h264" or args.expected_audio != "opus"):
        parser.error("the WHIP publisher uses Chrome H264/Opus")
    args.rtsp_publish_transport = "tcp"
    run = Run(args)
    result = {"head": benchmark_head(), "direction": args.direction, "status": "FAIL"}
    started = time.monotonic()
    try:
        with sync_playwright() as playwright, (
            run.publisher("rtmp") if args.direction == "whep" else contextlib.nullcontext()
        ):
            browser = playwright.chromium.launch(
                executable_path=args.browser,
                headless=True,
                args=["--autoplay-policy=no-user-gesture-required", "--use-fake-device-for-media-stream",
                      "--use-fake-ui-for-media-stream"],
            )
            try:
                page = browser.new_page()
                page.goto(f"http://127.0.0.1:{run.http_port}/")
                result["browser_version"] = browser.version
                result["resource"] = page.evaluate(
                    """async direction => {
                        document.body.innerHTML = '<video autoplay muted playsinline></video>';
                        const video = document.querySelector('video');
                        const peer = window.peer = new RTCPeerConnection();
                        if (direction === 'whip') {
                            const media = window.media = await navigator.mediaDevices.getUserMedia({video: true, audio: true});
                            for (const track of media.getTracks()) peer.addTrack(track, media);
                            const codecs = RTCRtpSender.getCapabilities('video').codecs.filter(codec => codec.mimeType === 'video/H264');
                            if (!codecs.length) throw new Error('Chrome has no H264 encoder');
                            peer.getTransceivers().find(item => item.sender.track.kind === 'video').setCodecPreferences(codecs);
                        } else {
                            peer.addTransceiver('video', {direction: 'recvonly'});
                            peer.addTransceiver('audio', {direction: 'recvonly'});
                            peer.ontrack = event => {
                                const media = video.srcObject || new MediaStream();
                                media.addTrack(event.track);
                                video.srcObject = media;
                                void video.play();
                            };
                        }
                        await peer.setLocalDescription(await peer.createOffer());
                        if (peer.iceGatheringState !== 'complete') {
                            await new Promise((resolve, reject) => {
                                const timer = setTimeout(() => reject(new Error('ICE gathering timeout')), 10000);
                                peer.onicegatheringstatechange = () => {
                                    if (peer.iceGatheringState === 'complete') {clearTimeout(timer); resolve();}
                                };
                            });
                        }
                        const path = direction === 'whep' ? '/play/whep/live/perf0' : '/publish/whip/browser/real';
                        const response = await fetch(path, {method: 'POST', headers: {'Content-Type': 'application/sdp'}, body: peer.localDescription.sdp});
                        if (response.status !== 201) throw new Error(`WebRTC POST ${response.status}: ${await response.text()}`);
                        const resource = response.headers.get('Location');
                        const answer = await response.text();
                        window.offer = peer.localDescription.sdp;
                        window.answer = answer;
                        await peer.setRemoteDescription({type: 'answer', sdp: answer});
                        return resource;
                    }""", args.direction,
                )
                page.wait_for_function("peer.connectionState === 'connected'", timeout=15000)
                for label in ("offer", "answer"):
                    (args.output / f"{label}.sdp").write_text(page.evaluate(f"window.{label}"))
                if args.direction == "whip":
                    wait_for_stream("127.0.0.1", run.http_port, "browser/real")
                    result["before"] = page.evaluate("async () => [...(await peer.getStats()).values()]")
                    progress = args.output / "decode-progress.txt"
                    probe = run.launch("decode-browser-whip", [args.ffmpeg, "-hide_banner", "-loglevel", "info",
                        "-xerror", "-nostats", "-progress", progress,
                        "-rtsp_transport", "tcp", "-timeout", "5000000", "-i",
                        f"rtsp://127.0.0.1:{run.rtsp_port}/browser/real", "-map", "0:v:0", "-map", "0:a:0",
                        "-vf", "showinfo", "-af", "ashowinfo", "-t", args.duration, "-f", "null", "-"])
                    if args.resume_video:
                        page.wait_for_timeout(1000)
                        page.evaluate("""async () => {
                            const sender = peer.getSenders().find(item => item.track.kind === 'video');
                            let parameters = sender.getParameters();
                            parameters.encodings.forEach(item => item.active = false);
                            await sender.setParameters(parameters);
                            parameters = sender.getParameters();
                            parameters.encodings.forEach(item => item.active = true);
                            await sender.setParameters(parameters);
                        }""")
                    result["publisher_pause_resume"] = args.resume_video
                    result["decode_timed_out"] = False
                    try:
                        result["decode_returncode"] = probe.wait(timeout=args.duration + 20)
                    except subprocess.TimeoutExpired:
                        result["decode_timed_out"] = True
                        stop_process(probe)
                        result["decode_returncode"] = probe.returncode
                    result["after"] = page.evaluate("async () => [...(await peer.getStats()).values()]")
                    text = probe.log_path.read_text()
                    frames = [int(value) for value in re.findall(r"^frame=(\d+)$", progress.read_text(), re.M)] if progress.exists() else []
                    result["decoded_frames"] = max(frames, default=0)
                    result["timestamps"] = {}
                    for kind, marker in (("video", "showinfo"), ("audio", "ashowinfo")):
                        pts = [float(value) for value in re.findall(r"\[Parsed_" + marker + r"_[^\]]*\].*?pts_time:([\d.eE+-]+)", text)]
                        if pts:
                            result["timestamps"][kind] = {"count": len(pts), "first": pts[0], "last": pts[-1],
                                "non_monotonic": sum(b < a for a, b in zip(pts, pts[1:]))}
                    assert not result["decode_timed_out"] and result["decode_returncode"] == 0, text
                    assert result["decoded_frames"] > 0 and "Video: h264" in text and "Audio: aac" in text, text
                    for kind in ("video", "audio"):
                        timestamps = result["timestamps"].get(kind, {})
                        assert timestamps.get("count", 0) > 1 and timestamps["non_monotonic"] == 0, (kind, timestamps)
                        assert timestamps["last"] - timestamps["first"] >= args.duration - 0.5, (kind, timestamps)
                        before = next(item for item in result["before"] if item["type"] == "outbound-rtp" and item["kind"] == kind)
                        after = next(item for item in result["after"] if item["type"] == "outbound-rtp" and item["kind"] == kind)
                        assert after["packetsSent"] > before["packetsSent"], (kind, before, after)
                    result["decode_verified"] = ["h264", "aac"]
                else:
                    checks = [("video", "framesDecoded")]
                    if args.expected_audio != "none":
                        checks.append(("audio", "totalSamplesReceived"))
                    deadline = time.monotonic() + 15
                    while time.monotonic() < deadline:
                        result["before"] = page.evaluate("async () => [...(await peer.getStats()).values()]")
                        if all(any(item.get("kind") == kind and item.get(field, 0) > 0
                                   for item in result["before"] if item["type"] == "inbound-rtp")
                               for kind, field in checks):
                            break
                        page.wait_for_timeout(100)
                    else:
                        raise AssertionError(("Chrome received no decoded audio/video", result["before"]))
                    page.wait_for_timeout(args.duration * 1000)
                    result["after"] = page.evaluate("async () => [...(await peer.getStats()).values()]")
                    for kind, field in checks:
                        before = next(item for item in result["before"] if item["type"] == "inbound-rtp" and item["kind"] == kind)
                        after = next(item for item in result["after"] if item["type"] == "inbound-rtp" and item["kind"] == kind)
                        assert after[field] > before[field], (kind, before, after)
                        codec = next(item for item in result["after"] if item["id"] == after["codecId"])
                        assert codec["mimeType"].lower() == {
                            "video": "video/" + args.expected_video, "audio": "audio/" + args.expected_audio}[kind], codec
                    assert any(item.get("dtlsState") == "connected" for item in result["after"]), result["after"]
                    result["decode_verified"] = [args.expected_video] + ([] if args.expected_audio == "none" else [args.expected_audio])
                assert request(run.http_port, "DELETE", result["resource"])[0] == 204
                assert request(run.http_port, "DELETE", result["resource"])[0] == 404
                page.evaluate("peer.close(); if (window.media) media.getTracks().forEach(track => track.stop())")
                result["duration_seconds"] = time.monotonic() - started
                result["status"] = "PASS"
            finally:
                browser.close()
    except Exception as error:
        result["error"] = f"{type(error).__name__}: {error}"
        raise
    finally:
        try:
            run.close()
        except Exception as error:
            result["status"] = "FAIL"
            result["cleanup_error"] = f"{type(error).__name__}: {error}"
            raise
        finally:
            (args.output / "commands.json").write_text(json.dumps(run.records, indent=2) + "\n")
            (args.output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(f"{args.direction} Chrome media encode/decode: PASS", flush=True)


if __name__ == "__main__":
    main()
