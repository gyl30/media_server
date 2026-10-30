#!/usr/bin/env python3
"""Classify `perf report --stdio --no-children -g none --sort dso,sym -F period,dso,sym` output of a
profile_run.sh recording into cost categories, as absolute Gcycles/s over the recording window.

Kernel network symbols include work done on behalf of loopback peers (see the report's loopback note): a loopback send
runs the receiver's softirq (net_rx_action) in the sender's context, so INCLUSIVE lists that share separately."""

import argparse
import re
import subprocess
from collections import Counter

CATEGORIES = [
    ("idle", r"^(intel_idle|cpuidle_|poll_idle|do_idle|mwait_idle|acpi_idle)"),
    ("fdk-aac encode", r"FDK|^dit_fft|^dct_I|^mdct_block|^LdData|^fixp_|^CalcInvLd|^CalcLd|^schur|^FDKsbr|^qmfAnalysis|^aacEnc|^FDKhybrid|^getScalefactor"),
    ("libopus encode", r"^(celt_|opus_|silk_|tonality_|analysis_|spreading_|op_pvq_search|find_best_pitch|quant_|encode_pulses|is_digital_silence|pitch_|dual_inner_prod|compute_|exp_rotation|alg_quant|intensity_stereo|stereo_|run_prefilter|transient_|dynalloc_|ec_enc|ec_laplace|clt_mdct_forward|haar1|downmix_|tf_|preemph|patch_transient|normalise_bands|denormalise|anti_collapse|renormalise|l1_metric|amp2Log2|interp_bits2pulses|clt_compute|hysteresis|median_of|process_|gen_|smooth_fade|frame_size_select|fft_|kf_bfly|ec_tell|unquant_|ec_dec|frac_div|isqrt|celt_rcp|_celt)"),
    ("ffmpeg opus decode", r"^(pvq_decode_band|ff_opus|ff_celt|celt_|ff_silk|silk_|opus_decode|ff_tx_|ff_vector_fmul|ff_opus_deemphasis|ff_celt_postfilter|quant_band|cos_|ff_mdct)"),
    ("ffmpeg aac decode", r"^(decode_spectrum|ff_aac|decode_ics|decode_band|spectral_to_sample|imdct_and_window|apply_tns|decode_scalefactors|aac_decode|ff_sine|ff_kbd|windowing_and_mdct|decode_cpe|decode_channel)"),
    ("swresample", r"^(swr_|resample_|ff_resample|convert|swri_|ff_audio_convert|conv_)"),
    ("srtp/crypto", r"aesni|^gcm_|ghash|^aes_|^srtp_|^bitvector|^AES_|^EVP_|^CRYPTO_|^ossl_|^hmac|^sha1|^srtp|^cipher"),
    ("h264/h265 parse", r"^(mpeg4_|h264|h265|hevc|annexb|avc_|nalu)"),
    ("memory/alloc", r"^(__mem|mem|malloc|free|calloc|realloc|operator new|operator delete|je_|_je|arena_|tcache|imalloc|ifree|sdallocx|extent_|rallocx|mallocx|__libc_malloc|__libc_free|cfree)"),
    ("asio/scheduling", r"boost::asio|^epoll|futex|^__pthread|pthread_|^__lll|std::condition|sched_yield|syscall"),
]
KERNEL_CATEGORIES = [
    ("idle", r"^(intel_idle|cpuidle|poll_idle|do_idle|mwait|acpi_idle|default_idle)"),
    ("kernel net rx (incl. loopback peer)", r"^(__netif_receive|netif_receive|net_rx_action|__napi|process_backlog|ip_rcv|ip_local_deliver|ip_protocol_deliver|udp_rcv|udp_queue_rcv|__udp4_lib_rcv|__udp_enqueue|udp_unicast_rcv|tcp_v4_rcv|tcp_v4_do_rcv|tcp_rcv|tcp_data_queue|tcp_ack|tcp_validate|tcp_event_data_recv|sock_def_readable|__skb_datagram_iter|skb_copy_datagram|udp_recvmsg|tcp_recvmsg|__skb_recv|sk_filter|tcp_queue_rcv|tcp_add_backlog|tcp_clean_rtx|enqueue_to_backlog|netif_rx|__netif_rx|__sock_queue_rcv|sock_queue_rcv|ip_sublist_rcv|ip_list_rcv|udp_queue_rcv_one_skb|__udp_queue_rcv)"),
    ("kernel net tx", r"^(udp_sendmsg|udp_send_skb|ip_make_skb|__ip_append_data|ip_append_data|ip_finish_output|__ip_finish_output|ip_output|ip_local_out|__ip_local_out|ip_send_skb|__dev_queue_xmit|dev_hard_start_xmit|loopback_xmit|tcp_sendmsg|tcp_write_xmit|__tcp_transmit_skb|tcp_push|tcp_sendmsg_locked|__tcp_push|ip_queue_xmit|__ip_queue_xmit|sock_sendmsg|____sys_sendmsg|___sys_sendmsg|__sys_sendmsg|__sys_sendmmsg|sendmmsg|ip_route_output|fib_|__ip_route|validate_xmit|skb_page_frag|sk_page_frag|alloc_skb|__alloc_skb|sock_alloc_send|skb_set_owner|ip_generic_getfrag|csum_partial|udp4_hwcsum|ip_setup_cork|skb_copy_to_page|tcp_rate|tcp_tso|tcp_current_mss|tcp_skb_entail|tcp_stream_alloc|ipv4_mtu|dst_release|nf_|__x64_sys_send|__x64_sys_write|sock_write_iter|tcp_chrono|tcp_event_new_data|tcp_schedule|tcp_wfree|sock_wfree|__sk_mem)"),
    ("kernel memory copy", r"^(rep_movs|copy_user|_copy_from|_copy_to|copy_from_iter|copy_to_iter|memcpy|memset|clear_page|clear_bhb|__memcpy|memmove|skb_copy_bits)"),
    ("kernel sched/syscall/epoll", r"^(__schedule|schedule|psi_|pick_|enqueue_|dequeue_|update_|try_to_wake|ttwu|select_task|switch_|__switch|finish_task|ep_|do_epoll|__x64_sys_epoll|futex|__futex|do_futex|entry_SYSCALL|syscall_|do_syscall|__fdget|fdget|fput|__fget|ktime_get|hrtimer|native_|raw_spin|_raw_spin|__raw|__wake_up|wake_up|irq|__irq|common_interrupt|sysvec|asm_sysvec|__sysvec|tick_|rcu_|exit_to_user|__rseq|rseq|restore_|error_entry|cpuacct|account_|__cgroup|cgroup|load_balance|newidle|sched_|set_next|put_prev|check_preempt|resched|reweight|avg_vruntime|vruntime|__enqueue|__dequeue|wakeup|timerqueue|__hrtimer|lapic|x86_|perf_|__perf|amd_|intel_pmu)"),
]


INCLUSIVE = ["__x64_sys_sendmsg", "__x64_sys_sendmmsg", "__x64_sys_sendto", "__x64_sys_writev", "__x64_sys_write", "net_rx_action",
             "__x64_sys_recvfrom", "__x64_sys_recvmsg", "__x64_sys_read", "__x64_sys_epoll_wait", "__x64_sys_futex"]


def classify(dso, sym):
    if dso.startswith("[kernel"):
        for name, pattern in KERNEL_CATEGORIES:
            if re.search(pattern, sym):
                return name
        return "kernel other"
    for name, pattern in CATEGORIES:
        if re.search(pattern, sym):
            return name
    return "other user"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("data")
    parser.add_argument("--seconds", type=float, default=15)
    parser.add_argument("--top", type=int, default=0, help="also list the top N unclassified symbols")
    args = parser.parse_args()
    report = subprocess.run(["perf", "report", "--stdio", "--no-children", "-g", "none", "--sort", "dso,sym", "-F", "period,dso,sym", "-i", args.data],
                            capture_output=True, text=True).stdout
    totals, other = Counter(), Counter()
    for line in report.splitlines():
        m = re.match(r"^\s+(\d+)\s+(\S+)\s+\[[.k]\]\s+(.*?)\s*$", line)
        if not m:
            continue
        period, dso, sym = int(m.group(1)), m.group(2), m.group(3)
        category = classify(dso, sym)
        totals[category] += period
        if category in ("other user", "kernel other"):
            other[f"{dso} {sym}"] += period
    total = sum(totals.values())
    busy = total - totals["idle"]
    print(f"total {total / 1e9 / args.seconds:.2f} Gcycles/s, excluding idle {busy / 1e9 / args.seconds:.2f}")
    print("| category | Gcycles/s | share of non-idle |")
    print("|---|---|---|")
    for name, value in totals.most_common():
        if name != "idle":
            print(f"| {name} | {value / 1e9 / args.seconds:.3f} | {100 * value / busy:.1f}% |")
    children = subprocess.run(["perf", "report", "--stdio", "--children", "-g", "none", "--sort", "sym", "-i", args.data],
                              capture_output=True, text=True).stdout
    inclusive = {}
    for line in children.splitlines():
        m = re.match(r"^\s+([\d.]+)%\s+[\d.]+%\s+\[k\]\s+(\S+)\s*$", line)
        if m and m.group(2) in INCLUSIVE:
            inclusive[m.group(2)] = float(m.group(1)) * total / 100
    print("| kernel entry (inclusive) | Gcycles/s |")
    print("|---|---|")
    for name in INCLUSIVE:
        if name in inclusive:
            print(f"| {name} | {inclusive[name] / 1e9 / args.seconds:.3f} |")
    for sym, value in other.most_common(args.top):
        print(f"  {value / 1e9 / args.seconds:.3f} {sym}")


if __name__ == "__main__":
    main()
