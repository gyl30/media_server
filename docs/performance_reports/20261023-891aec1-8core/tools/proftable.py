#!/usr/bin/env python3
import re
import subprocess
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
LINE = re.compile(r"^\s*([\d.]+)%\s+(user: other \((?:[^()]|\([^)]*\))*\)(?=\s|$)|\S.*?(?=\s{2,}|$))")
COLUMNS = (
    ("send", ("kernel: send syscalls",)),
    ("rx", ("kernel: softirq net rx",)),
    ("recv", ("kernel: recv syscalls", "kernel: epoll")),
    ("fault", ("kernel: page faults", "kernel: mm syscalls")),
    ("kother", ("kernel: other", "kernel: futex")),
    ("mux", ("user: mux/packetize",)),
    ("crypto", ("user: srtp/crypto", "user: other (libcrypto.so.3)")),
    ("alloc", ("user: allocator", "user: other (libjemalloc.so.2)")),
    ("copy", ("user: memcpy/memset",)),
)


def shares(directory):
    output = subprocess.run([sys.executable, TOOLS / "stackcat.py", directory / "perf-script.txt.gz"], capture_output=True, text=True, check=True).stdout
    result = {}
    for line in output.splitlines():
        match = LINE.match(line)
        if match:
            result[match.group(2).strip()] = float(match.group(1))
    return result


def perf_stat(directory):
    values = {}
    for line in (directory / "perf-stat.txt").read_text().splitlines():
        fields = line.split("\t")
        if len(fields) > 2 and re.fullmatch(r"[0-9.]+", fields[0]):
            values[fields[2]] = float(fields[0])
    return values


def main():
    print("| profile | viewers | Gcycles/s | Mcycles/viewer·s | " + " | ".join(name for name, _ in COLUMNS) + " | user other |")
    print("| --- " * (len(COLUMNS) + 5) + "|")
    for argument in sys.argv[1:]:
        name, viewers = argument.split(":")
        directory = TOOLS.parent / "raw" / "prof2" / name
        share = shares(directory)
        cycles = perf_stat(directory)["cycles"] / 15
        cells = []
        used = 0.0
        for _, categories in COLUMNS:
            value = sum(share.get(category, 0.0) for category in categories)
            used += value
            cells.append(f"{value:.1f}%")
        rest = sum(value for key, value in share.items() if key.startswith("user")) - sum(
            sum(share.get(category, 0.0) for category in categories) for column, categories in COLUMNS if column in ("mux", "crypto", "alloc", "copy"))
        print(f"| {name} | {viewers} | {cycles / 1e9:.2f} | {cycles / 1e6 / int(viewers):.2f} | " + " | ".join(cells) + f" | {rest:.1f}% |")


if __name__ == "__main__":
    main()
