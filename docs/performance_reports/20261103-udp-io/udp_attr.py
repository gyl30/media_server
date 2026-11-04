#!/usr/bin/env python3
"""Classify `perf script` samples of the server CPUs (perf record -a -C 0-7 -g) into UDP send, loopback delivery, receive and user-space
cost; send syscalls that reach tcp_sendmsg (RTMP, RTSP, HTTP-FLV, GB TCP) are reported as "tcp". Derived from 20261023-891aec1-8core/tools/stackcat.py, with these differences: net_rx_action reached inside a send syscall is
reported separately as "tx: loopback delivery" (on loopback the sender's syscall also runs the receiver's softirq; on a real NIC that
work happens on the receiving host and is replaced by driver transmit and completion), the send path is split by the frame nearest
to the leaf into syscall entry/exit, routing, IP ID, skb allocation and free, copy and checksum, security hooks, locks and the
rest of the stack, and user-space time is split into transport, SRTP, transcoding, muxing, allocation and copy. Usage:
udp_attr.py perf-script.txt[.gz] [cpu list]"""

import gzip
import re
import sys
from collections import Counter, defaultdict

HEADER = re.compile(r"^(\S.*?)\s+(\d+)\s+\[(\d+)\]\s+[\d.]+:\s+(\d+)\s+(\S+):")
FRAME = re.compile(r"^\s+[0-9a-f]+\s+(.+?)\s+\((.+)\)$")
SEND = ("__x64_sys_sendto", "__x64_sys_sendmsg", "__x64_sys_sendmmsg")
OTHER_SEND = ("__x64_sys_write", "__x64_sys_writev")
RECV = ("__x64_sys_recvfrom", "__x64_sys_recvmsg", "__x64_sys_recvmmsg", "__x64_sys_read", "__x64_sys_readv")
TX_PARTS = (
    ("syscall entry/exit", re.compile(r"^(entry_SYSCALL|syscall_exit|do_syscall_64|clear_bhb_loop|syscall_return|exit_to_user|__x64_sys_send|__sys_send|fpregs|switch_fpu)")),
    ("route", re.compile(r"^(fib_|ip_route_output|__ip_route_output|ip_route_output_key_hash|dst_release|dst_|rt_|xfrm_lookup|__mkroute|ipv4_mtu|ip4_datagram)")),
    ("ip id", re.compile(r"^(__ip_select_ident|ip_idents_reserve|siphash)")),
    ("skb alloc/free", re.compile(r"^(__alloc_skb|alloc_skb|kmem_cache|kmalloc|__kmalloc|kfree|skb_release|__kfree_skb|consume_skb|sock_wfree|skb_set_owner_w|sock_alloc_send|__slab|slab_|___slab|kmalloc_reserve|skb_free|napi_skb_cache|skb_attempt_defer|__build_skb|page_frag|memcg_|obj_cgroup|mod_objcg|refill_obj|__memcg|check_heap_object|__check_heap_object|__check_object_size)")),
    ("copy/csum", re.compile(r"^(copy_|_copy_|__copy|rep_movs|memcpy|memset|csum|ip_generic_getfrag|skb_copy|memmove|__memcpy|__memset|memset_orig)")),
    ("security", re.compile(r"^(aa_|apparmor|security_|selinux|bpf_lsm|lsm_)")),
    ("lock", re.compile(r"^(_raw_|__raw_|queued_spin|native_queued|_spin|__rcu_read|rcu_read)")),
)
USER_GROUPS = (
    ("transcode", re.compile(r"opus|celt|silk|pvq|FDK|fdk|aac|Aac|dct_|dit_fft|quant_|spreading|tonality|swr_|resample|ff_|avcodec|alg_quant|op_pvq|exp_rotation|clt_mdct|pitch_|FIXP|Fixp|fixp")),
    ("srtp/crypto", re.compile(r"srtp|aes|AES|hmac|sha1|SHA1|gcm|GCM|ossl|EVP_|CRYPTO_|bitvector|rdbx")),
    ("transport/session send", re.compile(r"udp_transport|send_rtp|send_udp|send_packet|whep_session|gb28181_udp_sender|socket_ops|reactive_socket")),
    ("memcpy/memset", re.compile(r"__mem(move|cpy|set)|memcpy|memmove|memset")),
    ("allocator", re.compile(r"^(je_|malloc|free|_int_|operator new|operator delete|cfree|realloc|calloc|tcache|arena_|extent_|sdallocx|rallocx|mallocx)")),
    ("asio runtime", re.compile(r"boost::asio::detail::(scheduler|epoll_reactor|executor_op|descriptor|wait_handler|completion_handler|strand)")),
    ("shared_ptr", re.compile(r"_Sp_counted_base|shared_ptr|weak_ptr")),
    ("mux/packetize", re.compile(r"rtmp_|rtp_|ps_|mpeg|flv_|rtsp_|hls|ts_|webrtc_packetizer|packetiz|nalu|h264_|h265_|avc|hevc|sink_dispatcher|media_stream")),
)


def strip(symbol):
    return re.sub(r"\+0x[0-9a-f]+$", "", symbol)


def tx_part(leaf):
    for name, pattern in TX_PARTS:
        if pattern.search(leaf):
            return name
    return "stack other"


def classify(comm, frames):
    symbols = [symbol for symbol, _ in frames]
    top_symbol, top_dso = frames[0] if frames else ("?", "?")
    if comm.startswith("swapper") or any(symbol.startswith(("intel_idle", "cpuidle_enter", "do_idle")) for symbol in symbols):
        return "idle", top_symbol
    in_send = any(symbol.startswith(SEND) for symbol in symbols)
    in_softirq = any(symbol.startswith(("net_rx_action", "process_backlog")) for symbol in symbols)
    if in_send and any(symbol.startswith(("tcp_sendmsg", "tcp_write_xmit", "__tcp_push")) for symbol in symbols):
        return "tcp: send (incl. loopback delivery)", top_symbol
    if in_send and in_softirq:
        return "tx: loopback delivery", top_symbol
    if in_send:
        return f"tx: {tx_part(top_symbol)}", top_symbol
    if in_softirq:
        return "rx: softirq (not in send)", top_symbol
    if any(symbol.startswith(RECV) for symbol in symbols):
        return "rx: recv syscalls", top_symbol
    if any(symbol.startswith(OTHER_SEND) for symbol in symbols):
        return "kernel: write/writev", top_symbol
    if any("epoll" in symbol for symbol in symbols) and top_dso == "[kernel.kallsyms]":
        return "kernel: epoll", top_symbol
    if top_dso == "[kernel.kallsyms]":
        if any(symbol.startswith("asm_exc_page_fault") for symbol in symbols):
            return "kernel: page faults", top_symbol
        if any("futex" in symbol for symbol in symbols):
            return "kernel: futex", top_symbol
        if any(symbol.startswith(("__x64_sys_madvise", "__x64_sys_munmap", "__x64_sys_mmap", "__x64_sys_mprotect")) for symbol in symbols):
            return "kernel: mm syscalls", top_symbol
        if any(symbol.startswith(("asm_sysvec_apic_timer", "hrtimer", "tick_", "__sysvec")) for symbol in symbols):
            return "kernel: timer/irq", top_symbol
        return "kernel: other", top_symbol
    for name, pattern in USER_GROUPS:
        if pattern.search(top_symbol):
            return f"user: {name}", top_symbol
    if "media_server" in top_dso:
        return "user: other media_server", top_symbol
    return f"user: other ({top_dso.rsplit('/', 1)[-1]})", top_symbol


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
        category, symbol = classify(comm, [(strip(symbol), dso) for symbol, dso in frames])
        totals[category] += period
        symbols[category][symbol[:60]] += period

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
    groups = Counter()
    for category, value in totals.items():
        if category != "idle":
            groups[category.split(":")[0]] += value
    print("groups: " + ", ".join(f"{name} {value * 100 / busy:.1f}%" for name, value in groups.most_common()))
    for category, value in sorted(totals.items(), key=lambda item: (item[0].split(":")[0], -item[1])):
        if category == "idle":
            continue
        top = ", ".join(f"{symbol} {count * 100 / busy:.1f}" for symbol, count in symbols[category].most_common(4))
        print(f"{value * 100 / busy:5.1f}%  {category:34s} {top}")


if __name__ == "__main__":
    main()
