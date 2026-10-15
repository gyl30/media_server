#include <cerrno>
#include <utility>
#include <algorithm>

#include <QScopeGuard>
#include <QMutexLocker>

#include "decode_thread.h"

extern "C"
{
#include <libavutil/time.h>
#include <libavutil/error.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
}

namespace media_server
{

namespace
{

QString error_text(int code)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, buffer, sizeof(buffer));
    return QString::fromUtf8(buffer);
}

}    // namespace

decode_thread::decode_thread(QUrl url, QSize output_size, QObject* parent) : QThread(parent), url_(std::move(url)), output_size_(output_size) {}

std::vector<decoded_video_frame> decode_thread::take_video_frames()
{
    QMutexLocker lock(&mutex_);
    std::vector<decoded_video_frame> frames;
    frames.swap(video_frames_);
    return frames;
}

std::vector<decoded_audio_block> decode_thread::take_audio_blocks()
{
    QMutexLocker lock(&mutex_);
    std::vector<decoded_audio_block> blocks;
    blocks.swap(audio_blocks_);
    pcm_bytes_ = 0;
    return blocks;
}

decoder_snapshot decode_thread::snapshot() const
{
    QMutexLocker lock(&mutex_);
    auto result = stats_;
    result.ended = isFinished();
    return result;
}

void decode_thread::set_output_size(QSize size)
{
    if (size.isEmpty())
    {
        return;
    }
    QMutexLocker lock(&mutex_);
    output_size_ = size;
}

int decode_thread::interrupt(void* opaque)
{
    const auto* self = static_cast<decode_thread*>(opaque);
    return self->isInterruptionRequested() || av_gettime_relative() >= self->io_deadline_us_;
}

void decode_thread::run()
{
    AVFormatContext* format = avformat_alloc_context();
    AVCodecContext* video = nullptr;
    AVCodecContext* audio = nullptr;
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    SwsContext* scaler = nullptr;
    SwrContext* resampler = nullptr;
    AVDictionary* options = nullptr;
    QString error;
    const auto cleanup = qScopeGuard(
        [&]()
        {
            av_dict_free(&options);
            swr_free(&resampler);
            sws_freeContext(scaler);
            av_frame_free(&frame);
            av_packet_free(&packet);
            avcodec_free_context(&audio);
            avcodec_free_context(&video);
            avformat_close_input(&format);
            QMutexLocker lock(&mutex_);
            stats_.error = error;
        });
    if (!format || !packet || !frame)
    {
        error = tr("无法分配媒体解码资源");
        return;
    }
    {
        QMutexLocker lock(&mutex_);
        if (output_size_.isEmpty())
        {
            error = tr("视频输出尺寸无效");
            return;
        }
    }
    format->interrupt_callback = AVIOInterruptCB{&decode_thread::interrupt, this};
    if (url_.scheme() == QStringLiteral("rtsp"))
    {
        // 轨道探测期间的数据不回放到实时显示队列。
        format->flags |= AVFMT_FLAG_NOBUFFER;
    }
    io_deadline_us_ = av_gettime_relative() + 15000000;
    av_dict_set(&options, "rtsp_transport", "tcp", 0);
    const QByteArray input = url_.isLocalFile() ? url_.toLocalFile().toUtf8() : url_.toEncoded();
    int result = avformat_open_input(&format, input.constData(), nullptr, &options);
    if (result < 0)
    {
        if (!isInterruptionRequested())
        {
            error = av_gettime_relative() >= io_deadline_us_ || result == AVERROR(ETIMEDOUT) ? tr("连接媒体超时")
                                                                                             : tr("打开媒体失败：%1").arg(error_text(result));
        }
        return;
    }
    result = avformat_find_stream_info(format, nullptr);
    if (result < 0)
    {
        if (!isInterruptionRequested())
        {
            error = av_gettime_relative() >= io_deadline_us_ || result == AVERROR(ETIMEDOUT) ? tr("读取媒体轨道超时")
                                                                                             : tr("读取媒体轨道失败：%1").arg(error_text(result));
        }
        return;
    }
    const int video_index = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    const int audio_index = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, video_index, nullptr, 0);
    if (video_index < 0 || format->streams[video_index]->codecpar->codec_id != AV_CODEC_ID_H264)
    {
        error = tr("当前客户端仅支持 H.264 视频");
        return;
    }
    if (audio_index >= 0)
    {
        const auto codec = format->streams[audio_index]->codecpar->codec_id;
        if (codec != AV_CODEC_ID_AAC && codec != AV_CODEC_ID_PCM_ALAW && codec != AV_CODEC_ID_PCM_MULAW)
        {
            error = tr("不支持音频编码：%1").arg(QString::fromLatin1(avcodec_get_name(codec)));
            return;
        }
    }
    for (const int index : {video_index, audio_index})
    {
        if (index < 0)
        {
            continue;
        }
        const auto* parameters = format->streams[index]->codecpar;
        const auto* codec = avcodec_find_decoder(parameters->codec_id);
        auto*& context = index == video_index ? video : audio;
        context = avcodec_alloc_context3(codec);
        if (!codec || !context)
        {
            error = tr("无法创建解码器");
            return;
        }
        result = avcodec_parameters_to_context(context, parameters);
        if (result >= 0)
        {
            // 各画面独立解码，避免 FFmpeg 默认按整机核数为每路额外创建大量线程。
            context->thread_count = 1;
            context->pkt_timebase = format->streams[index]->time_base;
            result = avcodec_open2(context, codec, nullptr);
        }
        if (result < 0)
        {
            error = tr("初始化解码器失败：%1").arg(error_text(result));
            return;
        }
    }
    {
        QMutexLocker lock(&mutex_);
        stats_.video_codec = QString::fromLatin1(avcodec_get_name(video->codec_id));
        const auto rate = av_guess_frame_rate(format, format->streams[video_index], nullptr);
        stats_.video_fps = rate.num > 0 && rate.den > 0 ? av_q2d(rate) : 0;
        if (audio)
        {
            stats_.audio_codec = QString::fromLatin1(avcodec_get_name(audio->codec_id));
        }
    }

    int audio_input_rate = 0;
    int audio_input_format = -1;
    qint64 next_pcm_pts = no_media_timestamp;
    AVChannelLayout audio_input_layout{};
    const auto layout_cleanup = qScopeGuard([&]() { av_channel_layout_uninit(&audio_input_layout); });

    const auto enqueue_audio = [&](decoded_audio_block output)
    {
        if (output.pcm.isEmpty())
        {
            return;
        }
        next_pcm_pts = output.pts_us == no_media_timestamp ? no_media_timestamp : output.pts_us + output.pcm.size() * 1000000 / (48000 * 4);
        QMutexLocker lock(&mutex_);
        stats_.pcm_samples += output.pcm.size() / 4;
        constexpr qsizetype capacity = 48000 * 2 * 2 * 300 / 1000;
        while (!audio_blocks_.empty() && pcm_bytes_ + output.pcm.size() > capacity)
        {
            pcm_bytes_ -= audio_blocks_.front().pcm.size();
            audio_blocks_.erase(audio_blocks_.begin());
            ++stats_.audio_dropped;
        }
        if (output.pcm.size() > capacity)
        {
            const auto removed = output.pcm.size() - capacity;
            output.pcm.remove(0, removed);
            if (output.pts_us != no_media_timestamp)
            {
                output.pts_us += removed * 1000000 / (48000 * 4);
            }
            ++stats_.audio_dropped;
        }
        pcm_bytes_ += output.pcm.size();
        audio_blocks_.push_back(std::move(output));
    };

    qint64 video_deadline_us = av_gettime_relative() + 10000000;
    qint64 audio_deadline_us = video_deadline_us;
    const auto receive = [&](AVCodecContext* decoder, int index) -> bool
    {
        while (true)
        {
            const int received = avcodec_receive_frame(decoder, frame);
            if (received == AVERROR(EAGAIN) || received == AVERROR_EOF)
            {
                return true;
            }
            if (received < 0)
            {
                error = tr("媒体解码失败：%1").arg(error_text(received));
                return false;
            }
            const auto timestamp = frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;
            const qint64 pts =
                timestamp == AV_NOPTS_VALUE ? no_media_timestamp : av_rescale_q(timestamp, format->streams[index]->time_base, AVRational{1, 1000000});
            if (index == video_index)
            {
                video_deadline_us = av_gettime_relative() + 10000000;
                decoded_video_frame output;
                QSize bounds;
                {
                    QMutexLocker lock(&mutex_);
                    bounds = output_size_;
                }
                const auto size = QSize(frame->width, frame->height).scaled(bounds, Qt::KeepAspectRatio);
                output.image = QImage(size, QImage::Format_RGB32);
                output.pts_us = pts;
                output.duration_us =
                    frame->duration > 0 ? av_rescale_q(frame->duration, format->streams[index]->time_base, AVRational{1, 1000000}) : 0;
                scaler = sws_getCachedContext(scaler,
                                              frame->width,
                                              frame->height,
                                              static_cast<AVPixelFormat>(frame->format),
                                              size.width(),
                                              size.height(),
                                              AV_PIX_FMT_BGRA,
                                              SWS_FAST_BILINEAR,
                                              nullptr,
                                              nullptr,
                                              nullptr);
                if (!scaler || output.image.isNull())
                {
                    error = tr("无法创建视频画面");
                    return false;
                }
                uint8_t* data[] = {output.image.bits(), nullptr, nullptr, nullptr};
                int lines[] = {static_cast<int>(output.image.bytesPerLine()), 0, 0, 0};
                if (sws_scale(scaler, frame->data, frame->linesize, 0, frame->height, data, lines) != size.height())
                {
                    error = tr("视频像素转换失败");
                    return false;
                }
                QMutexLocker lock(&mutex_);
                if (stats_.video_frames == 0)
                {
                    stats_.first_video_pts_us = pts;
                }
                ++stats_.video_frames;
                stats_.last_video_pts_us = pts;
                stats_.video_width = frame->width;
                stats_.video_height = frame->height;
                if (video_frames_.size() == 4)
                {
                    video_frames_.erase(video_frames_.begin());
                    ++stats_.video_dropped;
                }
                video_frames_.push_back(std::move(output));
            }
            else
            {
                audio_deadline_us = av_gettime_relative() + 10000000;
                if (!resampler)
                {
                    const AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
                    result = swr_alloc_set_opts2(&resampler,
                                                 &stereo,
                                                 AV_SAMPLE_FMT_S16,
                                                 48000,
                                                 &frame->ch_layout,
                                                 static_cast<AVSampleFormat>(frame->format),
                                                 frame->sample_rate,
                                                 0,
                                                 nullptr);
                    if (result >= 0)
                    {
                        result = swr_init(resampler);
                    }
                    if (result >= 0)
                    {
                        result = av_channel_layout_copy(&audio_input_layout, &frame->ch_layout);
                    }
                    if (result < 0)
                    {
                        error = tr("音频重采样初始化失败：%1").arg(error_text(result));
                        return false;
                    }
                    audio_input_rate = frame->sample_rate;
                    audio_input_format = frame->format;
                }
                if (frame->sample_rate != audio_input_rate || frame->format != audio_input_format ||
                    av_channel_layout_compare(&audio_input_layout, &frame->ch_layout) != 0)
                {
                    error = tr("音频参数在播放期间发生变化");
                    return false;
                }
                const auto delay = swr_get_delay(resampler, audio_input_rate);
                const int output_samples = static_cast<int>(av_rescale_rnd(delay + frame->nb_samples, 48000, audio_input_rate, AV_ROUND_UP));
                decoded_audio_block output;
                output.pts_us = pts == no_media_timestamp ? no_media_timestamp
                                                          : pts - av_rescale_q(delay, AVRational{1, audio_input_rate}, AVRational{1, 1000000});
                const int bytes = av_samples_get_buffer_size(nullptr, 2, output_samples, AV_SAMPLE_FMT_S16, 1);
                if (bytes < 0)
                {
                    error = tr("音频输出大小无效：%1").arg(error_text(bytes));
                    return false;
                }
                output.pcm.resize(bytes);
                uint8_t* pcm[] = {reinterpret_cast<uint8_t*>(output.pcm.data())};
                const auto** input_data = const_cast<const uint8_t**>(frame->extended_data);
                const int converted = swr_convert(resampler, pcm, output_samples, input_data, frame->nb_samples);
                if (converted < 0)
                {
                    error = tr("音频重采样失败：%1").arg(error_text(converted));
                    return false;
                }
                output.pcm.resize(converted * 4);
                {
                    QMutexLocker lock(&mutex_);
                    if (stats_.audio_frames == 0)
                    {
                        stats_.first_audio_pts_us = pts;
                    }
                    ++stats_.audio_frames;
                    stats_.audio_samples += frame->nb_samples;
                    stats_.last_audio_pts_us = pts;
                    stats_.audio_sample_rate = frame->sample_rate;
                    stats_.audio_channels = frame->ch_layout.nb_channels;
                }
                enqueue_audio(std::move(output));
            }
            av_frame_unref(frame);
            if (isInterruptionRequested())
            {
                return true;
            }
        }
    };

    while (!isInterruptionRequested())
    {
        io_deadline_us_ = audio ? std::min(video_deadline_us, audio_deadline_us) : video_deadline_us;
        result = av_read_frame(format, packet);
        if (isInterruptionRequested())
        {
            return;
        }
        const auto now = av_gettime_relative();
        if (now >= video_deadline_us)
        {
            error = tr("视频解码无进展，已超时（10 秒）");
            return;
        }
        if (audio && now >= audio_deadline_us)
        {
            error = tr("音频解码无进展，已超时（10 秒）");
            return;
        }
        if (result == AVERROR(EAGAIN))
        {
            QThread::msleep(1);
            continue;
        }
        if (result == AVERROR_EOF)
        {
            for (const int index : {video_index, audio_index})
            {
                if (index < 0)
                {
                    continue;
                }
                auto* decoder = index == video_index ? video : audio;
                result = avcodec_send_packet(decoder, nullptr);
                if (result < 0 && result != AVERROR_EOF)
                {
                    error = tr("结束媒体解码失败：%1").arg(error_text(result));
                    return;
                }
                if (!receive(decoder, index))
                {
                    return;
                }
            }
            while (resampler && !isInterruptionRequested())
            {
                const int samples = swr_get_out_samples(resampler, 0);
                if (samples <= 0)
                {
                    break;
                }
                decoded_audio_block output;
                output.pts_us = next_pcm_pts;
                const int bytes = av_samples_get_buffer_size(nullptr, 2, samples, AV_SAMPLE_FMT_S16, 1);
                if (bytes < 0)
                {
                    error = tr("音频尾部大小无效：%1").arg(error_text(bytes));
                    return;
                }
                output.pcm.resize(bytes);
                uint8_t* pcm[] = {reinterpret_cast<uint8_t*>(output.pcm.data())};
                const int converted = swr_convert(resampler, pcm, samples, nullptr, 0);
                if (converted < 0)
                {
                    error = tr("结束音频重采样失败：%1").arg(error_text(converted));
                    return;
                }
                if (converted == 0)
                {
                    break;
                }
                output.pcm.resize(converted * 4);
                enqueue_audio(std::move(output));
            }
            return;
        }
        if (result < 0)
        {
            error = tr("读取媒体失败：%1").arg(error_text(result));
            return;
        }
        const int index = packet->stream_index;
        if (index != video_index && index != audio_index)
        {
            av_packet_unref(packet);
            continue;
        }
        auto* decoder = index == video_index ? video : audio;
        result = avcodec_send_packet(decoder, packet);
        av_packet_unref(packet);
        if (result == AVERROR(EAGAIN))
        {
            error = tr("解码器未消费完上一媒体包");
            return;
        }
        if (result < 0)
        {
            error = tr("输入媒体解码失败：%1").arg(error_text(result));
            return;
        }
        if (!receive(decoder, index))
        {
            return;
        }
    }
}

}    // namespace media_server
