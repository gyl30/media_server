#!/usr/bin/env python3
"""Encode corpus files with aac and libfdk_aac via aacenc_check, decode the ADTS with ffmpeg and compare to the source.

Metrics (delay aligned): SNR, segmental SNR, log-spectral distance in 50 Hz-8 kHz, -40 dB bandwidth of source and decoded, encode speed.
"""
import argparse
import concurrent.futures
import glob
import json
import os
import subprocess

import numpy as np

CASES = [(48000, 2, 128000), (48000, 2, 96000), (48000, 2, 64000), (48000, 1, 64000), (48000, 1, 32000),
         (44100, 2, 128000), (32000, 2, 64000), (16000, 1, 32000)]
ENCODERS = ["aac", "libfdk_aac"]


def metrics(source, decoded, rate, channels, delay):
    x = source.reshape(-1, channels).mean(axis=1)
    y = decoded.reshape(-1, channels).mean(axis=1)[delay:]
    n = min(len(x), len(y))
    x, y = x[:n], y[:n]
    err = x - y
    snr = 10 * np.log10(np.sum(x * x) / max(np.sum(err * err), 1e-20))
    frame = 2048
    count = n // frame
    xs = x[:count * frame].reshape(count, frame)
    es = err[:count * frame].reshape(count, frame)
    energy = np.sum(xs * xs, axis=1)
    active = energy > frame * 1e-6
    seg = 10 * np.log10(np.maximum(energy[active], 1e-20) / np.maximum(np.sum(es[active] ** 2, axis=1), 1e-20))
    segsnr = float(np.mean(np.clip(seg, -10, 35))) if active.any() else None
    window = np.hanning(frame)
    xf = np.abs(np.fft.rfft(xs[active] * window, axis=1)) ** 2
    yf = np.abs(np.fft.rfft(y[:count * frame].reshape(count, frame)[active] * window, axis=1)) ** 2
    freqs = np.fft.rfftfreq(frame, 1 / rate)
    band = (freqs >= 50) & (freqs <= min(8000, rate / 2))
    lsd = float(np.mean(np.sqrt(np.mean((10 * np.log10(xf[:, band] + 1e-10) - 10 * np.log10(yf[:, band] + 1e-10)) ** 2, axis=1))))
    def bandwidth(power):
        spectrum = 10 * np.log10(np.mean(power, axis=0) + 1e-20)
        above = np.nonzero(spectrum > spectrum.max() - 40)[0]
        return float(freqs[above[-1]]) if len(above) else 0.0
    return {"snr_db": round(float(snr), 2), "segsnr_db": None if segsnr is None else round(segsnr, 2), "lsd_db": round(lsd, 2),
            "bandwidth_hz": round(bandwidth(yf)), "source_bandwidth_hz": round(bandwidth(xf))}


def run(binary, ffmpeg, path, rate, channels, bitrate, encoder, workdir, options):
    name = f"{os.path.basename(path)[:-4]}-{bitrate // 1000}k-{encoder}{'+' + options.replace(':', '+').replace('=', '') if options else ''}"
    adts = os.path.join(workdir, name + ".aac")
    out = subprocess.run([binary, path, str(rate), str(channels), str(bitrate), "30", ":".join(item for item in ("cutoff=20000" if rate >= 44100 else "", options) if item), adts],
                         capture_output=True, text=True, env={**os.environ, "AACENC": encoder}, timeout=600)
    fields = dict(item.split("=", 1) for item in out.stdout.split() if "=" in item)
    if fields.get("status") != "ok":
        return {"name": name, "status": fields.get("status", "crash")}
    decoded = subprocess.run([ffmpeg, "-v", "error", "-flags2", "skip_manual", "-i", adts, "-f", "f32le", "-ac", str(channels), "-ar", str(rate), "-"],
                             capture_output=True, timeout=600).stdout
    source = np.fromfile(path, dtype=np.float32)
    result = {"name": name, "status": "ok", "kbps": float(fields["kbps"]), "encode_ms_per_s": float(fields["encode_ms_per_s"]),
              "delay": int(fields["delay"])}
    result.update(metrics(source, np.frombuffer(decoded, dtype=np.float32), rate, channels, int(fields["delay"])))
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    parser.add_argument("--ffmpeg", required=True)
    parser.add_argument("--corpus", required=True)
    parser.add_argument("--workdir", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--encoders", default=",".join(ENCODERS))
    parser.add_argument("--options", default="", help="extra encoder options, e.g. afterburner=0")
    args = parser.parse_args()
    os.makedirs(args.workdir, exist_ok=True)
    jobs = []
    for path in sorted(glob.glob(os.path.join(args.corpus, "*.f32"))):
        _, rate, channels = os.path.basename(path)[:-4].rsplit("-", 2)
        for case in CASES:
            if case[:2] == (int(rate), int(channels)):
                for encoder in args.encoders.split(","):
                    jobs.append((path, *case, encoder))
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        results = list(pool.map(lambda job: run(args.binary, args.ffmpeg, *job, args.workdir, args.options), jobs))
    with open(args.output, "w") as handle:
        json.dump(results, handle, indent=1)
    rows = {}
    for item in results:
        if item["status"] != "ok":
            print("FAIL", item)
            continue
        key = item["name"].split("-", 1)[1].rsplit("-", 1)[0]
        rows.setdefault((key, item["name"].rsplit("-", 1)[1]), []).append(item)
    print("case encoder n snr segsnr lsd8k bandwidth source_bandwidth kbps encode_ms_per_s")
    for (key, encoder), items in sorted(rows.items()):
        mean = lambda field: np.mean([i[field] for i in items if i[field] is not None])
        print(f"{key} {encoder} {len(items)} {mean('snr_db'):.2f} {mean('segsnr_db'):.2f} {mean('lsd_db'):.2f} {mean('bandwidth_hz'):.0f} {mean('source_bandwidth_hz'):.0f} "
              f"{mean('kbps'):.1f} {mean('encode_ms_per_s'):.2f}")


if __name__ == "__main__":
    main()
