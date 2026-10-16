#include <cerrno>
#include <memory>
#include <vector>
#include <utility>
#include <algorithm>

#include <QFile>
#include <QScopeGuard>
#include <QMutexLocker>

#include "desktop/record_thread.h"
#include "desktop/finite_http_input.h"

extern "C"
{
#include <libavutil/time.h>
#include <libavutil/error.h>
#include <libavformat/avformat.h>
}

namespace media_server
{

namespace
{

constexpr qint64 open_timeout_us = 15'000'000;
constexpr qint64 track_timeout_us = 10'000'000;
constexpr int io_buffer_size = 65536;

QString error_text(int code)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, buffer, sizeof(buffer));
    return QString::fromUtf8(buffer);
}

int write_file(void* opaque, const uint8_t* buffer, int size)
{
    auto& file = *static_cast<QFile*>(opaque);
    return file.write(reinterpret_cast<const char*>(buffer), size) == size ? size : AVERROR(EIO);
}

int64_t seek_file(void* opaque, int64_t offset, int whence)
{
    auto& file = *static_cast<QFile*>(opaque);
    switch (whence & ~AVSEEK_FORCE)
    {
        case AVSEEK_SIZE:
            return file.size();
        case SEEK_CUR:
            offset += file.pos();
            break;
        case SEEK_END:
            offset += file.size();
            break;
        case SEEK_SET:
            break;
        default:
            return AVERROR(EINVAL);
    }
    return file.seek(offset) ? offset : AVERROR(EIO);
}

}    // namespace

record_thread::record_thread(QUrl url, QString target, playback_mode mode, QObject* parent)
    : QThread(parent), url_(std::move(url)), target_(std::move(target)), mode_(mode)
{
}

void record_thread::finish() { finish_requested_.store(true); }

record_snapshot record_thread::snapshot() const
{
    QMutexLocker lock(&mutex_);
    return stats_;
}

int record_thread::interrupt(void* opaque)
{
    const auto* self = static_cast<const record_thread*>(opaque);
    return self->isInterruptionRequested() || self->finish_requested_.load() || av_gettime_relative() >= self->io_deadline_us_;
}

void record_thread::run()
{
    const auto part = target_ + QStringLiteral(".part");
    AVFormatContext* input = avformat_alloc_context();
    AVFormatContext* output = nullptr;
    AVPacket* packet = av_packet_alloc();
    std::unique_ptr<finite_http_input> http_input;
    AVIOContext* input_io = nullptr;
    AVIOContext* output_io = nullptr;
    QFile file(part);
    bool created = false;
    bool saved = false;
    bool interrupted = false;
    QString error;
    const auto cleanup = qScopeGuard(
        [&]()
        {
            avformat_free_context(output);
            if (output_io)
            {
                av_freep(&output_io->buffer);
                avio_context_free(&output_io);
            }
            file.close();
            // 只删除本次以 NewOnly 新建的临时文件，不触碰已有目标或他人的 .part。
            if (created && !saved)
            {
                QFile::remove(part);
            }
            av_packet_free(&packet);
            avformat_close_input(&input);
            if (input_io)
            {
                av_freep(&input_io->buffer);
                avio_context_free(&input_io);
            }
            if (!saved && error.isEmpty() && finish_requested_.load() && !isInterruptionRequested())
            {
                error = tr("录制在收到视频前停止，未保存录像");
            }
            QMutexLocker lock(&mutex_);
            stats_.error = error;
            stats_.saved = saved;
            stats_.interrupted = interrupted;
        });
    if (!input || !packet)
    {
        error = tr("无法分配录制资源");
        return;
    }
    if (QFile::exists(target_))
    {
        error = tr("目标文件已存在");
        return;
    }
    input->interrupt_callback = AVIOInterruptCB{&record_thread::interrupt, this};
    io_deadline_us_ = av_gettime_relative() + open_timeout_us;
    const bool finite = mode_ == playback_mode::finite;
    if (finite && (url_.scheme() == QStringLiteral("http") || url_.scheme() == QStringLiteral("https")))
    {
        http_input = std::make_unique<finite_http_input>(*this, input->interrupt_callback);
        if (const auto opened = http_input->open(url_); opened < 0)
        {
            if (!isInterruptionRequested())
            {
                error = opened == AVERROR(ETIMEDOUT) ? tr("连接媒体超时") : tr("连接历史媒体失败：%1").arg(http_input->socket.errorString());
            }
            return;
        }
        auto* buffer = static_cast<uint8_t*>(av_malloc(io_buffer_size));
        input_io = buffer ? avio_alloc_context(buffer, io_buffer_size, 0, http_input.get(), &finite_http_input::read, nullptr, nullptr) : nullptr;
        if (!input_io)
        {
            av_free(buffer);
            error = tr("无法分配录制资源");
            return;
        }
        input->pb = input_io;
        input->flags |= AVFMT_FLAG_CUSTOM_IO;
    }
    AVDictionary* options = nullptr;
    av_dict_set(&options, "rtsp_transport", "tcp", 0);
    const QByteArray location = url_.isLocalFile() ? url_.toLocalFile().toUtf8() : url_.toEncoded();
    int result = avformat_open_input(&input, location.constData(), nullptr, &options);
    av_dict_free(&options);
    if (result < 0)
    {
        if (!isInterruptionRequested() && !finish_requested_.load())
        {
            error = av_gettime_relative() >= io_deadline_us_ ? tr("连接媒体超时") : tr("打开媒体失败：%1").arg(error_text(result));
        }
        return;
    }
    // 录制保留探测缓存，不使用实时显示的 NOBUFFER，避免丢掉首个关键帧。
    result = avformat_find_stream_info(input, nullptr);
    if (result < 0 || (input_io && input_io->error < 0 && input_io->error != AVERROR_EOF))
    {
        if (!isInterruptionRequested() && !finish_requested_.load())
        {
            error = tr("读取媒体轨道失败：%1").arg(error_text(result < 0 ? result : input_io->error));
        }
        return;
    }
    const int video_index = av_find_best_stream(input, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video_index < 0 || input->streams[video_index]->codecpar->codec_id != AV_CODEC_ID_H264)
    {
        error = tr("录制需要 H.264 视频");
        return;
    }
    const int audio_index = av_find_best_stream(input, AVMEDIA_TYPE_AUDIO, -1, video_index, nullptr, 0);
    if (audio_index >= 0)
    {
        const auto codec = input->streams[audio_index]->codecpar->codec_id;
        if (codec != AV_CODEC_ID_AAC && codec != AV_CODEC_ID_PCM_ALAW && codec != AV_CODEC_ID_PCM_MULAW)
        {
            error = tr("已声明的音频编码不支持录制：%1").arg(QString::fromUtf8(avcodec_get_name(codec)));
            return;
        }
    }
    {
        QMutexLocker lock(&mutex_);
        stats_.video_codec = QString::fromUtf8(avcodec_get_name(input->streams[video_index]->codecpar->codec_id));
        stats_.audio_codec = audio_index >= 0 ? QString::fromUtf8(avcodec_get_name(input->streams[audio_index]->codecpar->codec_id)) : QString{};
    }

    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly))
    {
        error = tr("无法新建临时录像文件：%1").arg(file.errorString());
        return;
    }
    created = true;
    auto* buffer = static_cast<uint8_t*>(av_malloc(io_buffer_size));
    output_io = buffer ? avio_alloc_context(buffer, io_buffer_size, 1, &file, nullptr, &write_file, &seek_file) : nullptr;
    if (!output_io || avformat_alloc_output_context2(&output, nullptr, "matroska", nullptr) < 0)
    {
        if (!output_io)
        {
            av_free(buffer);
        }
        error = tr("无法分配录制资源");
        return;
    }
    output->pb = output_io;
    output->flags |= AVFMT_FLAG_CUSTOM_IO;
    // 保留音视频相对偏移，把最早时间戳平移到 0。
    output->avoid_negative_ts = AVFMT_AVOID_NEG_TS_MAKE_ZERO;
    std::vector<int> mapping(input->nb_streams, -1);
    for (const int index : {video_index, audio_index})
    {
        if (index < 0)
        {
            continue;
        }
        auto* stream = avformat_new_stream(output, nullptr);
        if (!stream || avcodec_parameters_copy(stream->codecpar, input->streams[index]->codecpar) < 0)
        {
            error = tr("无法分配录制资源");
            return;
        }
        stream->codecpar->codec_tag = 0;
        stream->time_base = input->streams[index]->time_base;
        mapping[index] = stream->index;
    }
    result = avformat_write_header(output, nullptr);
    if (result < 0)
    {
        error = tr("写入录像文件失败：%1").arg(file.error() != QFileDevice::NoError ? file.errorString() : error_text(result));
        return;
    }

    // 有限下载按原样完整复制；只有实时录制从首个可解码关键帧写起。
    bool keyframe_seen = finite;
    bool video_written = false;
    bool source_ended = false;
    bool write_failed = false;
    qint64 first_video_us = no_media_timestamp;
    qint64 video_deadline_us = av_gettime_relative() + track_timeout_us;
    qint64 audio_deadline_us = video_deadline_us;
    while (!isInterruptionRequested() && !finish_requested_.load())
    {
        io_deadline_us_ = audio_index >= 0 ? std::min(video_deadline_us, audio_deadline_us) : video_deadline_us;
        result = av_read_frame(input, packet);
        if (isInterruptionRequested() || finish_requested_.load())
        {
            av_packet_unref(packet);
            break;
        }
        const auto now = av_gettime_relative();
        if (now >= video_deadline_us || (audio_index >= 0 && now >= audio_deadline_us))
        {
            av_packet_unref(packet);
            error = now >= video_deadline_us ? tr("视频无进展，已超时（10 秒）") : tr("音频无进展，已超时（10 秒）");
            break;
        }
        if (result == AVERROR(EAGAIN))
        {
            QThread::msleep(1);
            continue;
        }
        if (result == AVERROR_EOF)
        {
            // 有限输入还必须由 HTTP 解析确认完整结束，FFmpeg EOF 不足以证明。
            if (http_input && (!http_input->parser.is_done() || (input_io->error < 0 && input_io->error != AVERROR_EOF)))
            {
                error = tr("历史媒体传输未正常结束");
                break;
            }
            source_ended = true;
            break;
        }
        if (result < 0)
        {
            error = tr("读取媒体失败：%1").arg(error_text(result));
            break;
        }
        const int stream_index = packet->stream_index;
        // PS 可在探测后才出现新轨道；不能越界访问映射，也不能静默丢掉音频。
        const int output_index = static_cast<std::size_t>(stream_index) < mapping.size() ? mapping[static_cast<std::size_t>(stream_index)] : -1;
        if (output_index < 0 &&
            (static_cast<std::size_t>(stream_index) >= mapping.size() || input->streams[stream_index]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO))
        {
            av_packet_unref(packet);
            error = tr("录制开始后出现未识别的新轨道");
            break;
        }
        if (output_index < 0)
        {
            av_packet_unref(packet);
            continue;
        }
        if ((packet->flags & AV_PKT_FLAG_CORRUPT) != 0)
        {
            av_packet_unref(packet);
            error = tr("媒体包损坏");
            break;
        }
        if (stream_index == video_index)
        {
            video_deadline_us = now + track_timeout_us;
            // 实时源可能从 GOP 中途开始，此前的音视频包都不写入。
            keyframe_seen = keyframe_seen || (packet->flags & AV_PKT_FLAG_KEY) != 0;
        }
        else
        {
            audio_deadline_us = now + track_timeout_us;
        }
        if (!keyframe_seen)
        {
            av_packet_unref(packet);
            continue;
        }
        qint64 media_us = no_media_timestamp;
        if (stream_index == video_index && packet->pts != AV_NOPTS_VALUE)
        {
            const auto pts_us = av_rescale_q(packet->pts, input->streams[stream_index]->time_base, AVRational{1, 1000000});
            first_video_us = first_video_us == no_media_timestamp ? pts_us : first_video_us;
            media_us = pts_us - first_video_us;
        }
        av_packet_rescale_ts(packet, input->streams[stream_index]->time_base, output->streams[output_index]->time_base);
        packet->stream_index = output_index;
        packet->pos = -1;
        result = av_interleaved_write_frame(output, packet);
        if (result < 0)
        {
            write_failed = true;
            error = tr("写入录像文件失败：%1").arg(file.error() != QFileDevice::NoError ? file.errorString() : error_text(result));
            break;
        }
        video_written = video_written || stream_index == video_index;
        QMutexLocker lock(&mutex_);
        stats_.bytes = avio_tell(output->pb);
        stats_.media_us = media_us == no_media_timestamp ? stats_.media_us : media_us;
        ++(stream_index == video_index ? stats_.video_packets : stats_.audio_packets);
    }

    // 取消或写盘失败不保存；有限输入只有完整结束才保存；实时录制写入视频后停止、源结束或网络中断都保存。
    const bool cancelled = isInterruptionRequested();
    const bool keep = !cancelled && !write_failed && video_written && (finite ? source_ended : true);
    if (!keep)
    {
        if (error.isEmpty() && !cancelled && !video_written)
        {
            error = tr("未收到可写入的视频，未保存录像");
        }
        return;
    }
    result = av_write_trailer(output);
    if (result < 0 || output->pb->error < 0)
    {
        error = tr("写入录像文件收尾失败：%1")
                    .arg(file.error() != QFileDevice::NoError ? file.errorString() : error_text(result < 0 ? result : output->pb->error));
        return;
    }
    avio_flush(output->pb);
    file.close();
    if (file.error() != QFileDevice::NoError || output->pb->error < 0)
    {
        error = tr("写入录像文件收尾失败：%1").arg(file.errorString());
        return;
    }
    // 收尾期间收到的取消仍然有效，以改名提交为取消受理的终点。
    if (isInterruptionRequested())
    {
        return;
    }
    if (!QFile::rename(part, target_))
    {
        error = tr("录像文件改名失败，未保存");
        return;
    }
    saved = true;
    interrupted = !finite && !finish_requested_.load();
    QMutexLocker lock(&mutex_);
    stats_.bytes = QFile(target_).size();
}

}    // namespace media_server
