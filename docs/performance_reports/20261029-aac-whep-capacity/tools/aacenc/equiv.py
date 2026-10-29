#!/usr/bin/env python3
"""Run aacenc_check with two binaries over a signal/rate/channel/bitrate/option matrix and compare every result."""
import argparse
import concurrent.futures
import glob
import json
import os
import subprocess

SIGNALS = ["silence", "sine", "sweep", "noise", "impulse", "square", "fullscale", "overrange", "denormal", "tiny", "nan", "inf", "mixed"]
RATES = [8000, 11025, 16000, 22050, 24000, 32000, 44100, 48000, 88200, 96000]
OPTIONS = ["", "cutoff=20000", "aac_coder=fast", "aac_coder=anmr:strict=-2", "aac_pns=0", "aac_is=0", "aac_tns=0", "aac_ms=0", "aac_ms=1",
           "profile=aac_main", "profile=aac_ltp", "aac_pce=1"]


def cases(corpus):
    for signal in SIGNALS:
        for rate in RATES:
            for channels in (1, 2):
                for bitrate in (24000, 64000, 128000, 256000):
                    yield (signal, rate, channels, bitrate, 6, "")
    for path in sorted(glob.glob(os.path.join(corpus, "*.f32"))):
        name = os.path.basename(path)[:-4]
        _, rate, channels = name.rsplit("-", 2)
        rate, channels = int(rate), int(channels)
        bitrates = (32000, 64000) if channels == 1 or rate <= 16000 else (64000, 128000, 192000)
        for bitrate in bitrates:
            yield (path, rate, channels, bitrate, 30, "")
            if rate == 48000:
                yield (path, rate, channels, bitrate, 30, "cutoff=20000")
        if rate == 48000 and channels == 2:
            for option in OPTIONS[2:]:
                yield (path, rate, channels, 128000, 30, option)
    for signal in SIGNALS:
        for option in OPTIONS[1:]:
            yield (signal, 48000, 2, 128000, 6, option)


def run(binary, case):
    signal, rate, channels, bitrate, seconds, option = case
    out = subprocess.run([binary, signal, str(rate), str(channels), str(bitrate), str(seconds), option], capture_output=True, text=True, timeout=600)
    fields = dict(item.split("=", 1) for item in out.stdout.split())
    return fields


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--a", required=True)
    parser.add_argument("--b", required=True)
    parser.add_argument("--corpus", required=True)
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    all_cases = list(cases(args.corpus))
    results = []
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        futures = {pool.submit(lambda c: (c, run(args.a, c), run(args.b, c)), case): case for case in all_cases}
        for future in concurrent.futures.as_completed(futures):
            case, a, b = future.result()
            same = all(a.get(key) == b.get(key) for key in ("status", "ret", "packets", "bytes", "md5", "decode_errors", "decoded"))
            results.append({"case": [os.path.basename(str(case[0])), *case[1:]], "same": same, "a": a, "b": b})
    with open(args.output, "w") as handle:
        json.dump(results, handle)
    statuses = {}
    for item in results:
        key = (item["a"].get("status"), item["b"].get("status"))
        statuses[key] = statuses.get(key, 0) + 1
    differing = [item for item in results if not item["same"]]
    decode_errors = [item for item in results if item["a"].get("decode_errors") not in ("0", None)]
    print(f"cases={len(results)} identical={len(results) - len(differing)} differing={len(differing)} statuses={statuses} "
          f"decode_error_cases={len(decode_errors)}")
    for item in differing[:20]:
        print("DIFF", item["case"], item["a"], item["b"])


if __name__ == "__main__":
    main()
