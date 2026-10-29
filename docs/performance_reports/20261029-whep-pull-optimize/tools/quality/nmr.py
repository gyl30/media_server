#!/usr/bin/env python3
"""Simplified PEAQ-style noise-to-mask ratio between a reference and a coded signal (48 kHz stereo f32le).

Not a calibrated PEAQ/ODG implementation: masking uses Schroeder spreading over 0.5-Bark bands with a fixed
offset plus the Terhardt threshold in quiet (0 dBFS sine = 92 dB SPL). Only differences between encoders that
are fed the same reference are meaningful.
"""
import sys
import numpy as np

RATE = 48000
N = 2048
HOP = 1024
MASK_OFFSET_DB = 12.0


def load(path):
    data = np.fromfile(path, dtype=np.float32)
    return data[: len(data) // 2 * 2].reshape(-1, 2).astype(np.float64)


def align(reference, test):
    span = min(len(reference), len(test), RATE * 10)
    a = reference[:span].mean(axis=1)
    b = test[:span].mean(axis=1)
    size = 1 << int(np.ceil(np.log2(2 * span)))
    correlation = np.fft.irfft(np.fft.rfft(b, size) * np.conj(np.fft.rfft(a, size)), size)
    lags = np.concatenate([np.arange(0, 8192), np.arange(size - 8192, size)])
    best = lags[np.argmax(correlation[lags])]
    lag = best if best < size // 2 else best - size
    if lag >= 0:
        test = test[lag:]
    else:
        reference = reference[-lag:]
    length = min(len(reference), len(test))
    return reference[:length], test[:length], int(lag)


def bark(frequency):
    return 13 * np.arctan(0.00076 * frequency) + 3.5 * np.arctan((frequency / 7500.0) ** 2)


def threshold_in_quiet_db(frequency):
    f = np.maximum(frequency, 20) / 1000.0
    return 3.64 * f ** -0.8 - 6.5 * np.exp(-0.6 * (f - 3.3) ** 2) + 1e-3 * f ** 4


def analyse(reference, test):
    window = np.hanning(N)
    frequencies = np.fft.rfftfreq(N, 1 / RATE)
    z = bark(frequencies)
    edges = np.arange(0, bark(20000) + 0.5, 0.5)
    band = np.clip(np.digitize(z, edges) - 1, 0, len(edges) - 2)
    used = frequencies <= 20000
    bands = len(edges) - 1
    centers = edges[:-1] + 0.25
    dz = centers[:, None] - centers[None, :]
    spread_db = 15.81 + 7.5 * (dz + 0.474) - 17.5 * np.sqrt(1 + (dz + 0.474) ** 2)
    spread = 10 ** (spread_db / 10)
    full_scale = (N / 4) ** 2
    ath = np.zeros(bands)
    for b in range(bands):
        selected = (band == b) & used
        ath[b] = (10 ** ((threshold_in_quiet_db(frequencies[selected]) - 92) / 10)).min() * full_scale * selected.sum() if selected.any() else np.inf
    nmr_frames = []
    for channel in range(2):
        frames = (len(reference) - N) // HOP
        for index in range(frames):
            start = index * HOP
            r = np.fft.rfft(reference[start:start + N, channel] * window)
            t = np.fft.rfft(test[start:start + N, channel] * window)
            pr = np.bincount(band[used], weights=np.abs(r[used]) ** 2, minlength=bands)
            pe = np.bincount(band[used], weights=np.abs(r[used] - t[used]) ** 2, minlength=bands)
            mask = np.maximum(spread @ pr * 10 ** (-MASK_OFFSET_DB / 10), ath)
            nmr_frames.append(pe / mask)
    nmr = np.array(nmr_frames)
    total = 10 * np.log10(nmr.mean())
    disturbed = (10 * np.log10(nmr.max(axis=1) + 1e-30) > 1.5).mean() * 100
    noise = ((reference - test) ** 2).sum()
    snr = 10 * np.log10((reference ** 2).sum() / max(noise, 1e-30))
    return total, disturbed, snr


def main():
    reference = load(sys.argv[1])
    for path in sys.argv[2:]:
        r, t, lag = align(reference, load(path))
        trim = RATE // 2
        r, t = r[trim:-trim], t[trim:-trim]
        total, disturbed, snr = analyse(r, t)
        print(f"{path}: lag={lag} total_nmr_db={total:.2f} disturbed_frames_pct={disturbed:.1f} snr_db={snr:.2f}")


if __name__ == "__main__":
    main()
