// AAC encoder equivalence/validity harness.
// encoder: AACENC=aac (default) or AACENC=libfdk_aac (s16 interleaved input).
// usage: aacenc_check <signal|file.f32> <rate> <channels> <bitrate> <seconds> [encoder-opts] [out.adts]
// signals: silence sine sweep noise impulse square fullscale overrange denormal tiny nan inf mixed
// file.f32: interleaved float32 PCM at <rate>/<channels>.
// Prints: status, packet count, bytes, md5 of all packets, decoder errors, decoded SNR (dB, delay compensated).
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/md5.h>
#include <libavutil/opt.h>
#include <libavutil/dict.h>

static uint32_t lcg = 12345;
static float noise(void)
{
    lcg = lcg * 1664525u + 1013904223u;
    return (float)((int32_t)lcg) / 2147483648.0f;
}

static float *make_signal(const char *name, int rate, int channels, int seconds, long *frames)
{
    if (strstr(name, ".f32")) {
        FILE *f = fopen(name, "rb");
        if (!f)
            return NULL;
        fseek(f, 0, SEEK_END);
        long bytes = ftell(f);
        fseek(f, 0, SEEK_SET);
        long total = bytes / (long)sizeof(float);
        long limit = (long)rate * seconds * channels;
        if (seconds > 0 && total > limit)
            total = limit;
        float *data = malloc(total * sizeof(float));
        if (fread(data, sizeof(float), total, f) != (size_t)total)
            return NULL;
        fclose(f);
        *frames = total / channels;
        return data;
    }
    long n = (long)rate * seconds;
    float *data = calloc(n * channels, sizeof(float));
    for (long i = 0; i < n; i++) {
        double t = (double)i / rate;
        for (int c = 0; c < channels; c++) {
            float v = 0;
            if (!strcmp(name, "sine"))
                v = 0.5f * sinf(2 * M_PI * (440.0 + 110.0 * c) * t);
            else if (!strcmp(name, "sweep"))
                v = 0.7f * sinf(2 * M_PI * (20.0 * t + (rate / 2.0 - 20.0) * t * t / (2.0 * seconds)));
            else if (!strcmp(name, "noise"))
                v = 0.8f * noise();
            else if (!strcmp(name, "impulse"))
                v = (i % (rate / 7 + c * 31) == 0) ? 1.0f : 0.0f;
            else if (!strcmp(name, "square"))
                v = (fmod(t * (220.0 + 3 * c), 1.0) < 0.5) ? 0.99f : -0.99f;
            else if (!strcmp(name, "fullscale"))
                v = (i & 1) ? 1.0f : -1.0f;
            else if (!strcmp(name, "overrange"))
                v = 4.0f * sinf(2 * M_PI * 1000.0 * t) + 2.0f * noise();
            else if (!strcmp(name, "denormal"))
                v = 1e-40f * (float)((i + c) % 7 - 3);
            else if (!strcmp(name, "tiny"))
                v = 1e-6f * noise();
            else if (!strcmp(name, "nan"))
                v = (i == n / 2) ? NAN : 0.3f * sinf(2 * M_PI * 300.0 * t);
            else if (!strcmp(name, "inf"))
                v = (i == n / 2) ? INFINITY : 0.3f * sinf(2 * M_PI * 300.0 * t);
            else if (!strcmp(name, "mixed")) {
                double seg = fmod(t, 4.0);
                v = seg < 1 ? 0.0f : seg < 2 ? 0.9f * noise() : seg < 3 ? ((i % (rate / 3) < 64) ? 0.95f * noise() : 0.001f * noise())
                                                                         : 0.6f * sinf(2 * M_PI * 5000.0 * t);
            }
            data[i * channels + c] = v;
        }
    }
    *frames = n;
    return data;
}

int main(int argc, char **argv)
{
    if (argc < 6) {
        fprintf(stderr, "usage: %s <signal|file.f32> <rate> <channels> <bitrate> <seconds> [opts] [out.adts]\n", argv[0]);
        return 2;
    }
    const int rate = atoi(argv[2]), channels = atoi(argv[3]), bitrate = atoi(argv[4]), seconds = atoi(argv[5]);
    const char *opts = argc > 6 ? argv[6] : "";
    FILE *out = argc > 7 ? fopen(argv[7], "wb") : NULL;
    long frames = 0;
    float *pcm = make_signal(argv[1], rate, channels, seconds, &frames);
    if (!pcm) {
        printf("status=input_error\n");
        return 1;
    }

    const char *encoder_name = getenv("AACENC") ? getenv("AACENC") : "aac";
    const AVCodec *codec = avcodec_find_encoder_by_name(encoder_name);
    if (!codec) {
        printf("status=no_encoder\n");
        return 1;
    }
    AVCodecContext *enc = avcodec_alloc_context3(codec);
    enc->sample_rate = rate;
    enc->sample_fmt = codec->id == AV_CODEC_ID_AAC && strcmp(encoder_name, "aac") ? AV_SAMPLE_FMT_S16 : AV_SAMPLE_FMT_FLTP;
    enc->bit_rate = bitrate;
    enc->time_base = (AVRational){1, rate};
    enc->flags |= AV_CODEC_FLAG_BITEXACT;
    av_channel_layout_default(&enc->ch_layout, channels);
    AVDictionary *dict = NULL;
    av_dict_parse_string(&dict, opts, "=", ":", 0);
    enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    int ret = avcodec_open2(enc, codec, &dict);
    av_dict_free(&dict);
    if (ret < 0) {
        printf("status=open_error ret=%d\n", ret);
        return 1;
    }

    const AVCodec *dcodec = avcodec_find_decoder(AV_CODEC_ID_AAC);
    AVCodecContext *dec = avcodec_alloc_context3(dcodec);
    dec->extradata = av_mallocz(enc->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
    memcpy(dec->extradata, enc->extradata, enc->extradata_size);
    dec->extradata_size = enc->extradata_size;
    dec->flags |= AV_CODEC_FLAG_BITEXACT;
    dec->flags2 |= AV_CODEC_FLAG2_SKIP_MANUAL;
    if (avcodec_open2(dec, dcodec, NULL) < 0) {
        printf("status=decoder_open_error\n");
        return 1;
    }

    struct AVMD5 *md5 = av_md5_alloc();
    av_md5_init(md5);
    AVFrame *frame = av_frame_alloc(), *decoded = av_frame_alloc();
    AVPacket *pkt = av_packet_alloc();
    long packets = 0, bytes = 0, decode_errors = 0, pos = 0, out_frames = 0;
    float *recon = calloc((frames + 8 * enc->frame_size) * channels, sizeof(float));
    long recon_cap = frames + 8 * enc->frame_size;
    int status_error = 0;
    struct timespec t0, t1;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t0);
    double encode_seconds = 0;
    for (int flushing = 0;;) {
        if (!flushing) {
            if (pos >= frames) {
                flushing = 1;
                ret = avcodec_send_frame(enc, NULL);
            } else {
                int n = frames - pos < enc->frame_size ? (int)(frames - pos) : enc->frame_size;
                frame->nb_samples = n;
                frame->format = enc->sample_fmt;
                frame->sample_rate = rate;
                av_channel_layout_copy(&frame->ch_layout, &enc->ch_layout);
                frame->pts = pos;
                av_frame_get_buffer(frame, 0);
                for (int c = 0; c < channels; c++)
                    for (int i = 0; i < n; i++) {
                        float v = pcm[(pos + i) * channels + c];
                        if (enc->sample_fmt == AV_SAMPLE_FMT_S16) {
                            float scaled = isfinite(v) ? v * 32768.0f : 0.0f;
                            ((int16_t *)frame->extended_data[0])[i * channels + c] =
                                (int16_t)(scaled > 32767.0f ? 32767 : scaled < -32768.0f ? -32768 : lrintf(scaled));
                        } else
                            ((float *)frame->extended_data[c])[i] = v;
                    }
                pos += n;
                struct timespec a, b;
                clock_gettime(CLOCK_THREAD_CPUTIME_ID, &a);
                ret = avcodec_send_frame(enc, frame);
                clock_gettime(CLOCK_THREAD_CPUTIME_ID, &b);
                encode_seconds += (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9;
                av_frame_unref(frame);
            }
            if (ret < 0) {
                status_error = ret;
                break;
            }
        }
        for (;;) {
            struct timespec a, b;
            clock_gettime(CLOCK_THREAD_CPUTIME_ID, &a);
            ret = avcodec_receive_packet(enc, pkt);
            clock_gettime(CLOCK_THREAD_CPUTIME_ID, &b);
            encode_seconds += (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9;
            if (ret < 0)
                break;
            packets++;
            bytes += pkt->size;
            av_md5_update(md5, pkt->data, pkt->size);
            if (out) {
                uint8_t adts[7];
                int len = pkt->size + 7, sr = 0;
                static const int rates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};
                for (int k = 0; k < 13; k++)
                    if (rates[k] == rate)
                        sr = k;
                adts[0] = 0xFF;
                adts[1] = 0xF1;
                adts[2] = (uint8_t)((1 << 6) | (sr << 2) | (channels >> 2));
                adts[3] = (uint8_t)(((channels & 3) << 6) | (len >> 11));
                adts[4] = (uint8_t)((len >> 3) & 0xFF);
                adts[5] = (uint8_t)(((len & 7) << 5) | 0x1F);
                adts[6] = 0xFC;
                fwrite(adts, 1, 7, out);
                fwrite(pkt->data, 1, pkt->size, out);
            }
            if (avcodec_send_packet(dec, pkt) < 0)
                decode_errors++;
            av_packet_unref(pkt);
            while (avcodec_receive_frame(dec, decoded) == 0) {
                for (int i = 0; i < decoded->nb_samples && out_frames < recon_cap; i++, out_frames++)
                    for (int c = 0; c < channels; c++)
                        recon[out_frames * channels + c] = ((float *)decoded->extended_data[c])[i];
                av_frame_unref(decoded);
            }
        }
        if (ret == AVERROR_EOF)
            break;
    }
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t1);
    uint8_t digest[16];
    av_md5_final(md5, digest);
    char hex[33];
    for (int i = 0; i < 16; i++)
        sprintf(hex + 2 * i, "%02x", digest[i]);

    long delay = 0;
    double best = -1;
    const long probe_start = frames > 4L * rate ? 2L * rate : 0, probe_len = frames > 4L * rate ? 2L * rate : frames / 2;
    for (long lag = 0; lag <= 4096 && probe_start + lag + probe_len < out_frames; lag++) {
        double xy = 0, yy = 0;
        for (long i = probe_start; i < probe_start + probe_len; i++) {
            double x = 0, y = 0;
            for (int c = 0; c < channels; c++) {
                x += isfinite(pcm[i * channels + c]) ? pcm[i * channels + c] : 0;
                y += recon[(i + lag) * channels + c];
            }
            xy += x * y;
            yy += y * y;
        }
        double score = yy > 0 ? xy / sqrt(yy) : 0;
        if (score > best) {
            best = score;
            delay = lag;
        }
    }
    double sig = 0, err = 0;
    for (long i = 0; i + delay < out_frames && i < frames; i++)
        for (int c = 0; c < channels; c++) {
            double x = pcm[i * channels + c], y = recon[(i + delay) * channels + c];
            if (isfinite(x)) {
                sig += x * x;
                err += (x - y) * (x - y);
            }
        }
    double snr = err > 0 ? 10 * log10(sig / err) : (sig > 0 ? 999 : 0);
    printf("status=%s ret=%d packets=%ld bytes=%ld kbps=%.1f md5=%s decode_errors=%ld decoded=%ld snr_db=%.2f delay=%ld initial_padding=%d encode_ms_per_s=%.3f\n",
           status_error ? "encode_error" : "ok", status_error, packets, bytes, frames ? bytes * 8.0 / ((double)frames / rate) / 1000 : 0, hex,
           decode_errors, out_frames, snr, delay, enc->initial_padding, frames ? encode_seconds * 1000 / ((double)frames / rate) : 0);
    if (out)
        fclose(out);
    return 0;
}
