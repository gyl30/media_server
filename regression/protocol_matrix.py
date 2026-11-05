#!/usr/bin/env python3
"""Run bench/perf_run.py over the supported input/codec/output combinations on loopback and summarize the results.

RTSP AAC inputs carry a known GB output timestamp sawtooth: FFmpeg sends 33 AAC frames per RTP packet, the server's RTSP depacketizer
(third-party librtp/avpbs) steps the frames inside a packet by a whole 21 ms instead of 1024/48 ms and re-anchors on the next packet's RTP
timestamp, so every 33rd frame the GB audio timeline jumps by 32 x 1/3 ms with no lost samples. Those scenarios are XFAIL only when that
exact pattern is the sole failure; any other failure stays FAIL, and a clean run is reported as XPASS and also fails, so the exception
gets reviewed and removed."""

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ALL_OUTPUTS = "rtmp:2,rtsp:2,flv:2,hls:1,whep:2,gb_udp:1,gb_tcp_active:1,gb_tcp_passive:1"
NO_GB_OUTPUTS = "rtmp:2,rtsp:2,flv:2,hls:1,whep:2"
CHROME_OPUS_FMTP = "minptime=10;useinbandfec=1"

DERIVED = {
    "h265_aac.flv": ["-i", "avsync_720p30.flv", "-c:v", "libx265", "-preset", "fast", "-x265-params", "keyint=60:min-keyint=60:log-level=error",
                     "-b:v", "2M", "-c:a", "copy", "-f", "flv"],
    "h264_pcma.flv": ["-i", "avsync_720p30.flv", "-c:v", "copy", "-c:a", "pcm_alaw", "-ar", "8000", "-ac", "1", "-f", "flv"],
    "h264_pcmu.flv": ["-i", "avsync_720p30.flv", "-c:v", "copy", "-c:a", "pcm_mulaw", "-ar", "8000", "-ac", "1", "-f", "flv"],
    "h264_opus.flv": ["-i", "avsync_whip.mkv", "-c", "copy", "-f", "flv"],
}

# name, input, fixture, outputs, extra perf_run arguments, needs FFmpeg with Opus-in-FLV support, expected failure category of every group
SCENARIOS = [
    ("rtmp-h264-aac", "rtmp", "avsync_720p30.flv", ALL_OUTPUTS, [], False, None),
    ("rtmp-h264-bframes-aac", "rtmp", "avsync_bframes.flv", ALL_OUTPUTS, [], False, None),
    ("rtmp-h265-aac", "rtmp", "h265_aac.flv", ALL_OUTPUTS, [], False, None),
    ("rtmp-h264-pcma", "rtmp", "h264_pcma.flv", ALL_OUTPUTS, [], False, None),
    ("rtmp-h264-pcmu", "rtmp", "h264_pcmu.flv", ALL_OUTPUTS, [], False, None),
    ("rtmp-h264-opus", "rtmp", "h264_opus.flv", NO_GB_OUTPUTS, [], True, None),
    ("rtsp-tcp-h264-aac", "rtsp_tcp", "avsync_720p30.flv", ALL_OUTPUTS, [], False, None),
    ("rtsp-udp-h264-aac", "rtsp_udp", "avsync_720p30.flv", ALL_OUTPUTS, [], False, None),
    ("rtsp-tcp-h265-aac", "rtsp_tcp", "h265_aac.flv", ALL_OUTPUTS, [], False, None),
    ("rtsp-tcp-h264-opus", "rtsp_tcp", "avsync_whip.mkv", NO_GB_OUTPUTS, [], True, None),
    ("rtsp-tcp-h264-pcma", "rtsp_tcp", "h264_pcma.flv", ALL_OUTPUTS, [], False, None),
    ("whip-h264-opus", "whip", "avsync_720p30.flv", ALL_OUTPUTS, [], False, None),
    ("rtmp-pull", "rtmp_pull", "avsync_720p30.flv", ALL_OUTPUTS, [], False, None),
    ("rtsp-pull", "rtsp_pull", "avsync_720p30.flv", ALL_OUTPUTS, [], False, None),
    ("whep-pull", "whep_pull", "avsync_720p30.flv", ALL_OUTPUTS, [], False, None),
    ("whip-chrome-offer", "whip", "avsync_720p30.flv", "whep:2,flv:1", ["--whep-opus-fmtp", CHROME_OPUS_FMTP], False, None),
    ("whep-pull-chrome-offer", "whep_pull", "avsync_720p30.flv", "whep:2,flv:1", ["--whep-opus-fmtp", CHROME_OPUS_FMTP], False, None),
    ("whip-video-only-whep", "whip", "avsync_720p30.flv", "whep_video:2,whep:1,flv:1", [], False, None),
    ("whep-pull-video-only-whep", "whep_pull", "avsync_720p30.flv", "whep_video:2,whep:1,flv:1", [], False, None),
    ("gb-udp", "gb_udp", "avsync_720p30.flv", ALL_OUTPUTS, [], False, None),
    ("gb-tcp-active", "gb_tcp_active", "avsync_720p30.flv", ALL_OUTPUTS, [], False, None),
    ("gb-tcp-passive", "gb_tcp_passive", "avsync_720p30.flv", ALL_OUTPUTS, [], False, None),
    ("multi-source-cross-worker", "rtmp", "avsync_720p30.flv", "rtmp:20,rtsp:20,flv:20,hls:4,whep:12,gb_udp:8,gb_tcp_active:4,gb_tcp_passive:4",
     ["--sources", "4", "--churn", "rtmp:6"], False, None),
    ("slow-readers", "rtmp", "avsync_720p30.flv", "rtmp:2,rtsp:2,flv:2", ["--stalled-viewers", "2"], False, None),
    ("fault-source-stop", "rtmp", "avsync_720p30.flv", "rtmp:2,rtsp:2,flv:2,whep:2,gb_udp:1,gb_tcp_active:1,gb_tcp_passive:1", ["--fault", "source_stop"], False, "SOURCE_ENDED"),
    *[(f"fault-{fault.replace('_', '-')}-{output}", "rtmp", "avsync_720p30.flv", f"{output}:1", ["--fault", fault], False, "AUTHORIZATION")
      for fault in ("invalid_token", "replayed_token") for output in ("rtmp", "rtsp", "flv", "hls", "whep")],
]

RTSP_AAC_FRAMES_PER_PACKET = 33
RTSP_AAC_STEP_ERROR_MS = 1024 * 1000 / 48000 - 21
RTSP_AAC_SLIP_MS = (RTSP_AAC_FRAMES_PER_PACKET - 1) * RTSP_AAC_STEP_ERROR_MS
RTSP_AAC_TIMESTAMP_JUMPS = {"rtsp-tcp-h264-aac", "rtsp-udp-h264-aac", "rtsp-tcp-h265-aac"}


def rtsp_aac_sawtooth(probe):
    slips = probe.get("audio_pts_slips") or []
    frames = [slip["frame"] for slip in slips]
    return ([key for key, value in probe.get("checks", {}).items() if not value] == ["audio_timeline"] and probe.get("audio_codec") == "aac" and
            probe.get("audio_pts_overlaps") == 0 and bool(frames) and
            abs(probe.get("audio_pts_gaps", 0) - probe.get("audio_frames", 0) / RTSP_AAC_FRAMES_PER_PACKET) <= 1.5 and
            probe.get("audio_pts_slip_ms_min", -1) >= -RTSP_AAC_STEP_ERROR_MS - 0.01 and probe.get("audio_pts_slip_ms_max", 99) <= RTSP_AAC_SLIP_MS + 0.01 and
            all(abs(slip["ms"] - RTSP_AAC_SLIP_MS) <= 0.01 for slip in slips) and
            all(later - earlier == RTSP_AAC_FRAMES_PER_PACKET for earlier, later in zip(frames, frames[1:])))


def only_rtsp_aac_sawtooth(result):
    groups = result.get("groups", [])
    failing = {protocol: probe for protocol, probe in result.get("av_probe", {}).items() if not probe.get("ok")}
    return (result.get("status") == "FAIL" and result.get("capacity_boundary") == "UNCONFIRMED" and not result.get("errors") and bool(groups) and
            all(group["exit_code"] == 0 and group["measurement"].get("ready") == group["requested"] and
                group["measurement"].get("progressing") == group["requested"] and group["window"]["coverage"] >= 0.99 for group in groups) and
            not result.get("realtime_lagging_groups") and all(probe.get("ok") for probe in result.get("decode_probe", {}).values()) and
            not result.get("churn", {}).get("failed_rounds") and not result.get("source_churn", {}).get("failed_rounds") and bool(failing) and
            all(protocol.startswith("gb_") and rtsp_aac_sawtooth(probe) for protocol, probe in failing.items()))


def summarize(result):
    summary = {"status": result.get("status"), "boundary": result.get("capacity_boundary"), "errors": result.get("errors", [])[:3],
               "server_exit_before_cleanup": result.get("server_exit_before_cleanup"), "groups": {}, "av_probe": {}, "decode_probe": {}}
    for group in result.get("groups", []):
        measurement = group.get("measurement") or {}
        summary["groups"][group["name"]] = {key: measurement.get(key) for key in
                                            ("requested", "ready", "progressing", "viewer_realtime_min", "viewer_lag_span_ms_max")}
        summary["groups"][group["name"]]["failure_category"] = group.get("failure_category")
    for protocol, probe in (result.get("av_probe") or {}).items():
        summary["av_probe"][protocol] = {key: probe.get(key) for key in
                                         ("ok", "verdict", "decode_warnings", "video_frames", "audio_frames", "audio_rtp_packets", "audio_rtp_lost", "video_fps", "video_pts_backward", "video_pts_gaps", "audio_pts_gaps",
                                          "audio_pts_overlaps", "audio_pts_slip_ms_min", "audio_pts_slip_ms_max", "av_markers", "av_offset_ms_min", "av_offset_ms_p50", "av_offset_ms_max", "audio_codec", "audio_samples",
                                          "checks", "reference")}
    for protocol, probe in (result.get("decode_probe") or {}).items():
        summary["decode_probe"][protocol] = {"ok": probe.get("ok"),
                                             "streams": [f"{stream.get('codec_name')}:{stream.get('nb_read_frames')}" for stream in probe.get("streams", [])]}
    control = result.get("control_probe") or {}
    summary["control_ms_p99"] = control.get("ms_p99")
    summary["control_errors"] = control.get("errors")
    cleanup = result.get("cleanup") or {}
    summary["cleanup"] = {"remaining_receivers": cleanup.get("remaining_owned_receivers"), "reaped": cleanup.get("all_owned_processes_reaped"),
                          "fd_baseline": (result.get("baseline") or {}).get("fd"),
                          "fd_after": (cleanup.get("server_settled") or cleanup.get("server_after") or {}).get("fd")}
    summary["server_evidence"] = result.get("server_evidence")
    return summary


def expected_failure_observed(summary, category):
    groups = summary["groups"].values()
    cleanup = summary["cleanup"]
    return (summary["server_exit_before_cleanup"] is None and bool(groups) and all(group["failure_category"] == category for group in groups) and
            cleanup["remaining_receivers"] == [] and cleanup["reaped"] is True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True, help="new directory")
    parser.add_argument("--server-bin", type=Path, required=True)
    parser.add_argument("--client-dir", type=Path, required=True, help="release build directory with the benchmark clients")
    parser.add_argument("--fixtures", type=Path, required=True, help="directory with avsync_720p30.flv, avsync_bframes.flv, avsync_whip.mkv")
    parser.add_argument("--ffmpeg-bin", type=Path, required=True)
    parser.add_argument("--ffprobe-bin", type=Path, required=True)
    parser.add_argument("--opus-flv-ffmpeg-bin", type=Path, help="FFmpeg that muxes and demuxes Opus in FLV; scenarios needing it are skipped otherwise")
    parser.add_argument("--opus-flv-ffprobe-bin", type=Path)
    parser.add_argument("--only", nargs="*", help="scenario names")
    parser.add_argument("--duration", type=int, default=30)
    parser.add_argument("--workers", type=int, default=4)
    parser.add_argument("--server-cpus", default="0-3")
    parser.add_argument("--client-cpus", default="4-7")
    parser.add_argument("--ready-timeout", type=int, default=60)
    args = parser.parse_args()
    if args.output.exists():
        parser.error(f"output already exists: {args.output}")
    fixtures = args.output / "fixtures"
    fixtures.mkdir(parents=True)
    for name in ("avsync_720p30.flv", "avsync_bframes.flv", "avsync_whip.mkv"):
        (fixtures / name).symlink_to((args.fixtures / name).resolve())
    for name, command in DERIVED.items():
        ffmpeg = args.opus_flv_ffmpeg_bin if name == "h264_opus.flv" else args.ffmpeg_bin
        if ffmpeg is None:
            continue
        resolved = [str(fixtures / item) if item.endswith((".flv", ".mkv")) else item for item in command]
        subprocess.run([str(ffmpeg), "-v", "error", "-y", *resolved, str(fixtures / name)], check=True)

    summaries = {}
    failures = 0
    for name, input_name, fixture, outputs, extra, needs_opus_flv, expected_failure in SCENARIOS:
        if args.only and name not in args.only:
            continue
        ffmpeg, ffprobe = args.ffmpeg_bin, args.ffprobe_bin
        if needs_opus_flv:
            if args.opus_flv_ffmpeg_bin is None or args.opus_flv_ffprobe_bin is None:
                summaries[name] = {"status": "SKIPPED", "reason": "no FFmpeg with Opus in FLV"}
                continue
            ffmpeg, ffprobe = args.opus_flv_ffmpeg_bin, args.opus_flv_ffprobe_bin
        run = args.output / name
        command = [sys.executable, str(ROOT / "bench/perf_run.py"), "--output", str(run), "--input", input_name, "--outputs", outputs,
                   "--server-bin", str(args.server_bin), "--client-dir", str(args.client_dir), "--ffmpeg-bin", str(ffmpeg), "--ffprobe-bin", str(ffprobe),
                   "--fixture", str(fixtures / fixture), "--whip-fixture", str(fixtures / "avsync_whip.mkv"),
                   "--host", "127.0.0.10", "--client-address", "127.0.0.11", "--workers", str(args.workers),
                   "--server-cpus", args.server_cpus, "--client-cpus", args.client_cpus, "--warmup", "5", "--duration", str(args.duration),
                   "--ready-timeout", str(args.ready_timeout), "--av-probe", str(min(20, args.duration - 5)), "--decode-probe",
                   "--control-probe-ms", "500", "--cleanup-settle", "3", *extra]
        completed = subprocess.run(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=os.environ)
        (args.output / f"{name}.stdout").write_text(completed.stdout)
        result_path = run / "result.json"
        result = json.loads(result_path.read_text()) if result_path.is_file() else None
        summary = summarize(result) if result else {"status": "NO_RESULT"}
        summary["exit_code"] = completed.returncode
        if name in RTSP_AAC_TIMESTAMP_JUMPS and summary["status"] == "PASS":
            summary["status"] = "XPASS"
            summary["known_issue"] = "RTSP AAC GB timestamp sawtooth no longer observed; review and remove the exception"
        elif name in RTSP_AAC_TIMESTAMP_JUMPS and only_rtsp_aac_sawtooth(result):
            summary["status"] = "XFAIL"
            summary["known_issue"] = "RTSP AAC GB timestamp sawtooth: 21 ms steps inside 33-frame RTP packets, re-anchored each packet"
        if expected_failure:
            summary["expected_failure"] = expected_failure
            summary["status"] = "PASS" if expected_failure_observed(summary, expected_failure) else "FAIL"
        summaries[name] = summary
        failures += summary.get("status") not in ("PASS", "XFAIL")
        print(f"{name}: {summary.get('status')} {summary.get('boundary')} exit={completed.returncode}", flush=True)
    (args.output / "summary.json").write_text(json.dumps(summaries, indent=2))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
