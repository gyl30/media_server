#!/usr/bin/env python3
"""Summarise `ss -uanmp` output for media_server UDP sockets: even (RTP) and odd (RTCP) local ports in the media pool."""
import re
import sys

LINE = re.compile(r"^\S+\s+(\d+)\s+(\d+)\s+(\S+):(\d+)\s+\S+\s+users:\(\(\"media_server")
SKMEM = re.compile(r"skmem:\(r(\d+),rb(\d+),t(\d+),tb(\d+),f\d+,w\d+,o\d+,bl\d+,d(\d+)\)")


def summarise(text, address=None, low=49152):
    stats = {}
    lines = text.splitlines()
    for index, line in enumerate(lines):
        match = LINE.match(line.strip())
        if not match or index + 1 >= len(lines):
            continue
        port = int(match.group(4))
        if port < low or (address and match.group(3) != address):
            continue
        mem = SKMEM.search(lines[index + 1])
        if not mem:
            continue
        kind = "rtcp" if port % 2 else "rtp"
        item = stats.setdefault(kind, {"sockets": 0, "recvq": 0, "recvq_max": 0, "r": 0, "r_max": 0, "rb": 0, "nonempty": 0, "drops": 0, "t": 0})
        recvq, r, rb, t, drops = int(match.group(1)), int(mem.group(1)), int(mem.group(2)), int(mem.group(3)), int(mem.group(5))
        item["sockets"] += 1
        item["recvq"] += recvq
        item["recvq_max"] = max(item["recvq_max"], recvq)
        item["r"] += r
        item["r_max"] = max(item["r_max"], r)
        item["rb"] = rb
        item["t"] += t
        item["nonempty"] += r > 0
        item["drops"] += drops
    return stats


def main():
    arguments = sys.argv[1:]
    address = None
    if arguments[:1] == ["--address"]:
        address, arguments = arguments[1], arguments[2:]
    for path in arguments:
        stats = summarise(open(path, errors="replace").read(), address)
        print(path, " ".join(f"{kind}:" + ",".join(f"{key}={value}" for key, value in item.items()) for kind, item in sorted(stats.items())))


if __name__ == "__main__":
    main()
