#!/usr/bin/env python3
"""Decode PCMA RTP sent to the camera from a pcap and detect the predeclared tone pattern."""
import argparse
import audioop
import json
import subprocess

import numpy as np

FREQS = [500, 800, 1100, 1400, 1700]

parser = argparse.ArgumentParser()
parser.add_argument("pcap")
parser.add_argument("--src", required=True)
parser.add_argument("--dst", required=True)
parser.add_argument("--wav", default="")
args = parser.parse_args()

fields = subprocess.run(["tshark", "-r", args.pcap, "-Y", f"ip.src=={args.src} && ip.dst=={args.dst} && udp && !sip",
                         "-T", "fields", "-e", "frame.time_epoch", "-e", "udp.srcport", "-e", "udp.dstport", "-e", "udp.payload"],
                        capture_output=True, text=True, check=True).stdout.splitlines()
flows = {}
for line in fields:
    parts = line.split("\t")
    if len(parts) < 4 or not parts[3]:
        continue
    data = bytes.fromhex(parts[3].replace(":", ""))
    if len(data) < 12 or data[0] >> 6 != 2:
        continue
    pt = data[1] & 0x7F
    seq, ts, ssrc = int.from_bytes(data[2:4], "big"), int.from_bytes(data[4:8], "big"), int.from_bytes(data[8:12], "big")
    flows.setdefault((parts[1], parts[2], pt, ssrc), []).append((float(parts[0]), seq, ts, data[12:]))

report = {"flows": []}
for (sport, dport, pt, ssrc), packets in flows.items():
    entry = {"src_port": sport, "dst_port": dport, "pt": pt, "ssrc": ssrc, "packets": len(packets)}
    if pt != 8:
        report["flows"].append(entry)
        continue
    seqs = [p[1] for p in packets]
    tss = [p[2] for p in packets]
    entry.update(first_time=packets[0][0], last_time=packets[-1][0], duration=packets[-1][0] - packets[0][0],
                 payload_sizes=sorted({len(p[3]) for p in packets}),
                 seq_gaps=sum(1 for a, b in zip(seqs, seqs[1:]) if (b - a) & 0xFFFF != 1),
                 ts_steps=sorted({(b - a) & 0xFFFFFFFF for a, b in zip(tss, tss[1:])})[:5])
    pcm = np.frombuffer(audioop.alaw2lin(b"".join(p[3] for p in packets), 2), dtype="<i2").astype(np.float64)
    entry["rms"] = float(np.sqrt(np.mean(pcm ** 2))) if len(pcm) else 0
    entry["peak"] = float(np.max(np.abs(pcm))) if len(pcm) else 0
    window = 800
    labels = []
    for start in range(0, len(pcm) - window + 1, window):
        chunk = pcm[start:start + window] * np.hanning(window)
        spectrum = np.abs(np.fft.rfft(chunk))
        freqs = np.fft.rfftfreq(window, 1 / 8000)
        energy = [spectrum[np.argmin(np.abs(freqs - f))] for f in FREQS]
        total = np.sqrt(np.sum(spectrum ** 2)) + 1e-9
        best = int(np.argmax(energy))
        labels.append(FREQS[best] if energy[best] / total > 0.5 else 0)
    runs = []
    for label in labels:
        if runs and runs[-1][0] == label:
            runs[-1][1] += 1
        else:
            runs.append([label, 1])
    tones = [label for label, count in runs if label and count >= 2]
    expected = FREQS * (len(tones) // len(FREQS) + 2)
    best_match = 0
    for offset in range(len(FREQS)):
        match = 0
        for got, want in zip(tones, expected[offset:]):
            if got != want:
                break
            match += 1
        best_match = max(best_match, match)
    entry.update(windows_100ms=len(labels), tone_runs=runs[:40], tone_sequence=tones[:30], ordered_tones=best_match,
                 tone_windows=sum(1 for label in labels if label), silent_windows=sum(1 for label in labels if not label))
    if args.wav:
        import wave
        with wave.open(args.wav, "wb") as out:
            out.setnchannels(1)
            out.setsampwidth(2)
            out.setframerate(8000)
            out.writeframes(pcm.astype("<i2").tobytes())
    report["flows"].append(entry)
print(json.dumps(report, indent=1))
