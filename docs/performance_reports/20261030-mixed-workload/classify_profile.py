#!/usr/bin/env python3
"""Group `perf report --sort dso,sym` flat output (profmix.sh flat.txt) into cost categories."""
import re, sys, collections
rules = [
 ("opus_enc(libopus)", r"celt_encode|opus_encode|tonality|analysis_|run_analysis|^silk_|alg_quant|op_pvq_search|quant_partition|quant_band|spreading_decision|clt_mdct_forward|compute_theta|stereo_itheta|exp_rotation|haar1|pitch_|find_best_pitch|encode_pulses|opus_fft|celt_inner_prod|celt_pitch|transient_analysis|compute_band_energies|normalise_bands|quant_fine|quant_coarse|ec_enc|dynalloc|tf_analysis|celt_preemph|is_digital_silence|ec_laplace_encode|downmix|compute_mdcts|celt_lpc|_celt_autocorr|remove_doubling|icwrs|bitexact|interp_bits2pulses|compute_allocation|l1_metric|median_of|amp2Log2|stereo_analysis|anti_collapse|patch_transient|frac_mul|celt_fir|xcorr_kernel|silk|dual_inner_prod|renormalise|op_pvq|compute_vbr|hysteresis|opus_|_celt|celt_"),
 ("opus_dec(ffmpeg)", r"pvq_decode_band|ff_celt|ff_opus|ff_silk|celt_decode|opus_decode|celt_postfilter|celt_denormalize|silk_decode|ff_tx_mdct_inv.*15|pfa_15"),
 ("aac_enc(fdk)", r"^FDK|dit_fft|dct_IV|dct_II|fft_|qmfAnalysis|scaleValues|fixmul|CalcLdData|getScalefactor|FDKaacEnc|transportEnc|aacEnc|fLog2|schur|CLpc|FDKsbr|sbrEnc|psyMain|FDKfft|fft"),
 ("aac_dec(ffmpeg)", r"decode_spectrum|ff_aac|imdct|ff_tx_mdct_inv|spectral_to_sample|decode_ics|apply_tns|ff_sine|decode_cce|aac_decode|ff_tx_"),
 ("srtp/crypto", r"aes|ghash|srtp|gcm|sha1|hmac|EVP_|CRYPTO_|ossl|OPENSSL|bn_|BN_|ecp_"),
 ("h264/rtp/mux(ireader)", r"h264|h265|hevc|nalu|annexb|rtp_|rtcp_|flv_|mpeg_ts|mpeg_ps|ps_|ts_|rtsp_|rtmp_|amf|mov_|sdp_"),
 ("alloc", r"^malloc$|^free$|^calloc|^realloc|je_|arena_|tcache|extent_|^operator new|^operator delete|_int_malloc|cfree|sdallocx|rallocx|mallocx|imalloc|ifree|large_"),
 ("libc mem", r"memmove|memcpy|memset|memcmp|strlen"),
 ("asio/event", r"boost::asio|epoll|scheduler|reactive_|do_run_one|spawn|coro|fcontext|jump_fcontext|ontop_fcontext|strand"),
 ("shared_ptr/atomic", r"_Sp_counted|shared_ptr|weak_ptr|atomic"),
 ("mutex", r"pthread_mutex|futex|__lll"),
]
for path in sys.argv[1:]:
    cats = collections.Counter(); total = 0; other = collections.Counter()
    for line in open(path):
        m = re.match(r"\s*([\d.]+)%\s+(\S+)\s+\[(.)\]\s+(.*)", line)
        if not m: continue
        pct, dso, kind, sym = float(m.group(1)), m.group(2), m.group(3), m.group(4).strip()
        total += pct
        if kind == "k" or "kernel" in dso:
            cat = "kernel"
        else:
            name = re.sub(r"^(media_server::)", "", sym)
            cat = next((c for c, r in rules if re.search(r, name)), None)
            if cat is None:
                cat = "media_server(other)" if dso == "media_server" else dso
                other[(dso, sym[:90])] += pct
        cats[cat] += pct
    print(f"== {path.split('/')[-1]} total={total:.1f}")
    for c, v in cats.most_common(): print(f"  {c:28s} {v:6.2f}")
    print("  top other:", [(s, round(v,2)) for (d, s), v in other.most_common(12)])
