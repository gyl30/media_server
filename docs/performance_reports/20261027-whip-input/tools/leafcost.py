#!/usr/bin/env python3
import gzip
import re
import sys
from collections import Counter

RULES = [
    ("idle", r"intel_idle|poll_idle|cpuidle|mwait|do_idle|acpi_idle"),
    ("opus_decode", r"pvq_|opus|celt|silk|ff_tx_mdct_inv|pfa_15xM|deemphasis"),
    ("aac_encode", r"aac|quantiz|psy_|codebook|search_for|mark_pns|pow34|cbrtf|ff_tx_mdct_fwd|ff_tx_fft|lpc|__expf|__log2f|__logf|__cos|__sin|__powf|__exp2f|ff_vector_fmul|ff_scalarproduct|ff_kbd|_ff_aac|ff_init_nextband|tns|ms_|is_"),
    ("resample", r"swr|conv_AV_SAMPLE|resample|audio_fifo|av_samples"),
    ("srtp", r"srtp|aes|AES|gcm|sha1|SHA1|hmac|HMAC|bitvector|ossl|EVP_|CRYPTO_|v128"),
    ("h264_rtp_demux", r"h264|annexb|rtsp_demuxer|rtp_|avpkt2bs|rtcp|jitter|nalu|mpeg4_"),
    ("memcpy", r"memmove|memcpy|memset|rep_movs"),
    ("alloc", r"malloc|free|je_|jemalloc|tcache|arena|extent|cfree|operator new|operator delete|av_buffer|av_malloc|av_free|av_frame|av_packet"),
    ("kernel_net", r"udp|skb|sock|tcp|ip_|__dev|netif|net_|inet|copy_to_user|_copy_|recvmsg|sendmsg|loopback|nf_|__x64_sys_recv|__x64_sys_send|syscall|entry_SYSCALL|do_syscall|fdget|epoll|ep_"),
]


def category(symbol, kernel):
    for name, pattern in RULES:
        if re.search(pattern, symbol):
            return name if not (kernel and name not in ("idle", "kernel_net", "memcpy")) else ("kernel_net" if name != "idle" else "idle")
    return "kernel_other" if kernel else "user_other"


def main(path, threads=None):
    totals, symbols = Counter(), Counter()
    header = None
    with gzip.open(path, "rt", errors="replace") as handle:
        for line in handle:
            if not line.startswith("\t") and line.strip():
                match = re.match(r"(\S+)\s+(\d+)\s+\[(\d+)\]\s+[\d.]+:\s+(\d+)\s+cycles", line)
                header = match and (match.group(1), match.group(2), int(match.group(4)))
                leaf = True
                continue
            if header and leaf and line.strip():
                leaf = False
                fields = line.strip().split(" ", 1)
                symbol = fields[1] if len(fields) > 1 else "?"
                symbol = re.sub(r"\+0x[0-9a-f]+ ", " ", symbol)
                kernel = "[kernel.kallsyms]" in symbol
                name = symbol.split(" (")[0]
                cat = category(name, kernel) if header[0] == "media_server" or "idle" in name else "other_process"
                totals[cat] += header[2]
                if cat in ("user_other", "kernel_other"):
                    symbols[name] += header[2]
    busy = sum(value for key, value in totals.items() if key not in ("idle", "other_process"))
    print(f"{path}: busy Gcycles={busy / 1e9:.1f}")
    for key, value in totals.most_common():
        print(f"  {key:16} {value / 1e9:8.2f} G  {100 * value / busy if key not in ('idle', 'other_process') else 0:5.1f}%")
    print("  top other:", ", ".join(f"{name}={100 * value / busy:.2f}%" for name, value in symbols.most_common(15)))


main(sys.argv[1])
