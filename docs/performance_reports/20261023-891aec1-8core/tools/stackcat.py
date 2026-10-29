#!/usr/bin/env python3
import gzip
import re
import sys
from collections import Counter, defaultdict

HEADER = re.compile(r"^(\S.*?)\s+(\d+)\s+\[(\d+)\]\s+[\d.]+:\s+(\d+)\s+(\S+):")
FRAME = re.compile(r"^\s+[0-9a-f]+\s+(.+?)\s+\((.+)\)$")
SEND = ("__x64_sys_sendto", "__x64_sys_sendmsg", "__x64_sys_sendmmsg", "__x64_sys_write", "__x64_sys_writev")
RECV = ("__x64_sys_recvfrom", "__x64_sys_recvmsg", "__x64_sys_recvmmsg", "__x64_sys_read", "__x64_sys_readv")
USER_GROUPS = (
    ("memcpy/memset", re.compile(r"__mem(move|cpy|set)|memcpy|memmove|memset")),
    ("allocator", re.compile(r"^(je_|malloc|free|_int_|operator new|operator delete|cfree|realloc|calloc|tcache|arena_|extent_)")),
    ("srtp/crypto", re.compile(r"srtp|aes|AES|hmac|sha1|SHA1|gcm|GCM|ossl|EVP_|CRYPTO_")),
    ("asio runtime", re.compile(r"boost::asio::detail::(scheduler|epoll_reactor|executor_op|reactive_socket|descriptor|wait_handler|completion_handler|strand)")),
    ("shared_ptr", re.compile(r"_Sp_counted_base|shared_ptr|weak_ptr")),
    ("mux/packetize", re.compile(r"rtmp_|rtp_|ps_|mpeg|flv_|rtsp_|hls|ts_|webrtc_packetizer|packetiz|nalu|h264_|h265_|avc|hevc|aac|opus")),
)


def classify(comm, frames):
    symbols = [symbol for symbol, _ in frames]
    top_symbol, top_dso = frames[0] if frames else ("?", "?")
    if comm.startswith("swapper") or any(symbol.startswith(("intel_idle", "cpuidle_enter", "do_idle")) for symbol in symbols):
        return "idle", top_symbol
    if any(symbol.startswith(("net_rx_action", "process_backlog")) for symbol in symbols):
        return "kernel: softirq net rx", top_symbol
    if any(symbol.startswith(SEND) for symbol in symbols):
        return "kernel: send syscalls", top_symbol
    if any(symbol.startswith(RECV) for symbol in symbols):
        return "kernel: recv syscalls", top_symbol
    if any("epoll" in symbol for symbol in symbols) and top_dso == "[kernel.kallsyms]":
        return "kernel: epoll", top_symbol
    if top_dso == "[kernel.kallsyms]":
        if any(symbol.startswith("asm_exc_page_fault") for symbol in symbols):
            return "kernel: page faults", top_symbol
        if any("futex" in symbol for symbol in symbols):
            return "kernel: futex", top_symbol
        if any(symbol.startswith(("__x64_sys_madvise", "__x64_sys_munmap", "__x64_sys_mmap", "__x64_sys_mprotect")) for symbol in symbols):
            return "kernel: mm syscalls", top_symbol
        return "kernel: other", top_symbol
    clean = re.sub(r"\+0x[0-9a-f]+$", "", top_symbol)
    for name, pattern in USER_GROUPS:
        if pattern.search(clean):
            return f"user: {name}", clean
    if "media_server" in top_dso:
        return "user: other media_server", clean
    return f"user: other ({top_dso.rsplit('/', 1)[-1]})", clean


def main():
    path = sys.argv[1]
    cpus = set(range(8)) if len(sys.argv) < 3 else {int(item) for item in sys.argv[2].split(",")}
    opener = gzip.open if path.endswith(".gz") else open
    totals = Counter()
    symbols = defaultdict(Counter)
    header = None
    frames = []

    def flush():
        if header is None:
            return
        comm, cpu, period = header
        if cpu not in cpus:
            return
        category, symbol = classify(comm, [(re.sub(r"\+0x[0-9a-f]+$", "", symbol), dso) for symbol, dso in frames])
        totals[category] += period
        symbols[category][symbol[:70]] += period

    with opener(path, "rt", errors="replace") as source:
        for line in source:
            match = HEADER.match(line)
            if match:
                flush()
                header = (match.group(1), int(match.group(3)), int(match.group(4)))
                frames = []
                continue
            frame = FRAME.match(line)
            if frame and header is not None:
                frames.append((frame.group(1), frame.group(2)))
        flush()
    busy = sum(value for key, value in totals.items() if key != "idle")
    print(f"busy cycles sampled: {busy / 1e9:.1f}G")
    for category, value in totals.most_common():
        if category == "idle":
            continue
        top = ", ".join(f"{symbol} {count * 100 / busy:.1f}" for symbol, count in symbols[category].most_common(4))
        print(f"{value * 100 / busy:5.1f}%  {category:28s} {top}")


if __name__ == "__main__":
    main()
