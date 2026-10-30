#!/usr/bin/env python3
"""Check RTP sequence and Opus timestamp continuity in whep_fanout --dump-file output."""
import struct, sys, collections
for path in sys.argv[1:]:
    data = open(path, "rb").read()
    pos = 0
    last = {}
    stats = collections.defaultdict(lambda: collections.Counter())
    while pos + 11 <= len(data):
        ns, rtcp, size = struct.unpack_from("<qBH", data, pos)
        pkt = data[pos + 11: pos + 11 + size]
        pos += 11 + size
        if rtcp or len(pkt) < 12:
            continue
        pt = pkt[1] & 0x7F
        seq, ts, ssrc = struct.unpack_from("!HII", pkt, 2)
        s = stats[(pt, ssrc)]
        s["packets"] += 1
        if ssrc in last:
            ls, lt, ln = last[ssrc]
            if (seq - ls) & 0xFFFF != 1:
                s["seq_jump"] += 1
            d = (ts - lt) & 0xFFFFFFFF
            if pt == 111 and d not in (960,):
                s["ts_delta_%d" % d] += 1
            if pt == 111 and ns - ln > 60e6:
                s["arrival_gap_gt60ms"] += 1
        last[ssrc] = (seq, ts, ns)
    for k, v in stats.items():
        print(path.split("/")[-2], k, dict(v))
