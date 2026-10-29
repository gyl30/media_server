#!/usr/bin/env python3
import gzip
import re
import sys
from collections import Counter

HEADER = re.compile(r"^(\S.*?)\s+(\d+)\s+\[(\d+)\]\s+[\d.]+:\s+(\d+)\s+(\S+):")
FRAME = re.compile(r"^\s+[0-9a-f]+\s+(.+?)\s+\((.+)\)$")


def main():
    path, patterns = sys.argv[1], sys.argv[2:]
    totals = Counter()
    busy = 0
    header = None
    symbols = []

    def flush():
        nonlocal busy
        if header is None or header[0] > 7 or header[2]:
            return
        busy += header[1]
        for pattern in patterns:
            if any(re.match(pattern, symbol) for symbol in symbols):
                totals[pattern] += header[1]

    with gzip.open(path, "rt", errors="replace") as source:
        for line in source:
            match = HEADER.match(line)
            if match:
                flush()
                header = (int(match.group(3)), int(match.group(4)), match.group(1).startswith("swapper"))
                symbols = []
                continue
            frame = FRAME.match(line)
            if frame and header is not None:
                symbols.append(re.sub(r"\+0x[0-9a-f]+$", "", frame.group(1)))
        flush()
    for pattern in patterns:
        print(f"{totals[pattern] * 100 / busy:5.1f}%  {pattern}")


if __name__ == "__main__":
    main()
