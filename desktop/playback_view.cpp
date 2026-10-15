#include <cstdlib>
#include <utility>
#include <algorithm>

#include <SDL.h>
#include <QMimeData>
#include <QPainter>
#include <QJsonArray>
#include <QDropEvent>
#include <QDataStream>
#include <QJsonObject>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QDragEnterEvent>
#include <QCoreApplication>

#include "desktop/playback_view.h"
#include "desktop/platform_client.h"

namespace media_server
{

playback_view::playback_view(QWidget* parent) : QWidget(parent)
{
    setMinimumSize(160, 100);
    setAcceptDrops(true);
    set_status(QStringLiteral("双击右侧设备通道开始预览"));
    timer_.setInterval(10);
    connect(&timer_, &QTimer::timeout, this, &playback_view::update_media);
}

playback_view::~playback_view() { stop(); }

void playback_view::play(platform_client& client, const play_target& target)
{
    stop();
    final_stats_ = {};
    session_ = new QObject(this);
    set_status(QStringLiteral("%1 · 正在申请播放").arg(target.name));
    QString path = QStringLiteral("/api/play");
    QJsonObject body;
    if (target.type == play_target::kind::gb_channel)
    {
        path = QStringLiteral("/api/devices/%1/channels/%2/play")
                   .arg(QString::fromLatin1(QUrl::toPercentEncoding(target.id)), QString::fromLatin1(QUrl::toPercentEncoding(target.channel_id)));
    }
    else
    {
        const auto key = target.type == play_target::kind::source ? QStringLiteral("source_id") : QStringLiteral("push_device_id");
        body.insert(QStringLiteral("source"), QJsonObject{{key, target.id}});
    }
    request_ = client.request(
        "POST",
        path,
        body,
        session_,
        [this, &client, target](platform_response response)
        {
            request_.clear();
            if (!response.ok())
            {
                set_status(QStringLiteral("播放申请失败：%1，请手动重试").arg(response.error));
                return;
            }
            const QUrl url(response.document.object().value(QStringLiteral("rtsp_url")).toString(), QUrl::StrictMode);
            if (!url.isValid() || url.scheme() != QStringLiteral("rtsp") || url.host().isEmpty())
            {
                set_status(QStringLiteral("平台返回的 RTSP 地址无效"));
                return;
            }
            if (target.type == play_target::kind::gb_channel)
            {
                const auto stream_id = response.document.object().value(QStringLiteral("stream_id")).toString();
                if (stream_id.isEmpty())
                {
                    set_status(QStringLiteral("平台返回的国标点播标识无效"));
                    return;
                }
                set_status(QStringLiteral("%1 · 正在等待国标媒体就绪").arg(target.name));
                wait_for_gb_media(client, target, stream_id, url, QDeadlineTimer(15'000));
                return;
            }
            start_decoder(target.name, url);
        },
        target.type == play_target::kind::gb_channel ? 25'000 : 10'000);
}

void playback_view::wait_for_gb_media(
    platform_client& client, const play_target& target, const QString& stream_id, const QUrl& url, QDeadlineTimer deadline)
{
    if (deadline.hasExpired())
    {
        set_status(QStringLiteral("国标媒体未在15秒内就绪，请手动重试"));
        return;
    }
    const auto path = QStringLiteral("/api/devices/%1/channels").arg(QString::fromLatin1(QUrl::toPercentEncoding(target.id)));
    request_ = client.request(
        "GET",
        path,
        {},
        session_,
        [this, &client, target, stream_id, url, deadline](platform_response response)
        {
            request_.clear();
            if (deadline.hasExpired())
            {
                set_status(QStringLiteral("国标媒体未在15秒内就绪，请手动重试"));
                return;
            }
            if (!response.ok())
            {
                set_status(QStringLiteral("媒体就绪查询失败：%1").arg(response.error));
                return;
            }
            const auto channels = response.document.object().value(QStringLiteral("channels")).toArray();
            for (const auto& value : channels)
            {
                const auto channel = value.toObject();
                if (channel.value(QStringLiteral("channel_id")).toString() != target.channel_id)
                {
                    continue;
                }
                const auto live = channel.value(QStringLiteral("live")).toObject();
                if (live.value(QStringLiteral("live_id")).toString() != stream_id ||
                    live.value(QStringLiteral("state")).toString() != QStringLiteral("streaming"))
                {
                    set_status(QStringLiteral("点播已停止或媒体源已变化"));
                    return;
                }
                if (!live.value(QStringLiteral("media_ready")).isBool())
                {
                    set_status(QStringLiteral("平台缺少媒体就绪信息，请升级信令服务"));
                    return;
                }
                if (live.value(QStringLiteral("media_ready")).toBool())
                {
                    start_decoder(target.name, url);
                    return;
                }
                QTimer::singleShot(500,
                                   session_,
                                   [this, &client, target, stream_id, url, deadline]()
                                   { wait_for_gb_media(client, target, stream_id, url, deadline); });
                return;
            }
            set_status(QStringLiteral("点播已停止或媒体源已变化"));
        },
        static_cast<int>(std::max<qint64>(1, std::min<qint64>(5000, deadline.remainingTime()))));
}

void playback_view::start_decoder(const QString& name, const QUrl& url)
{
    // 解码线程独立保活至 finished，关闭画面不在 GUI 中等待网络线程。
    auto* decoder = new decode_thread(url, QSize(std::max(160, width()), std::max(100, height())), QCoreApplication::instance());
    decoder_ = decoder;
    connect(decoder,
            &QThread::finished,
            session_,
            [this, decoder]()
            {
                update_media();
                final_stats_ = decoder->snapshot();
                decoder_.clear();
                timer_.stop();
                reset_audio();
                set_status(final_stats_.error.isEmpty() ? QStringLiteral("媒体源已结束") : final_stats_.error + QStringLiteral("；请手动重试"));
            });
    connect(decoder, &QThread::finished, decoder, &QObject::deleteLater);
    set_status(QStringLiteral("%1 · 正在等待自然关键帧").arg(name));
    decoder->start();
    timer_.start();
}

void playback_view::stop()
{
    delete session_;
    session_ = nullptr;
    if (request_)
    {
        request_->abort();
        request_.clear();
    }
    if (decoder_)
    {
        final_stats_ = decoder_->snapshot();
        decoder_->requestInterruption();
        decoder_.clear();
    }
    timer_.stop();
    reset_audio();
    video_.clear();
    pcm_.clear();
    image_ = {};
    wall_pts_ = no_media_timestamp;
    shown_pts_ = no_media_timestamp;
    rendered_frames_ = 0;
    set_status(QStringLiteral("已停止"));
}

void playback_view::reset_audio()
{
    if (audio_id_ != 0)
    {
        SDL_PauseAudioDevice(audio_id_, 1);
        SDL_ClearQueuedAudio(audio_id_);
        audio_id_ = 0;
        emit audible_changed(false);
    }
    audio_period_bytes_ = 0;
    audio_end_pts_ = no_media_timestamp;
}

void playback_view::set_audible(quint32 audio_id, int period_bytes)
{
    if (audio_id_ == audio_id)
    {
        return;
    }
    reset_audio();
    pcm_.clear();
    const auto anchor = audio_id == 0 && !video_.empty() && video_.back().pts_us != no_media_timestamp ? video_.back().pts_us : shown_pts_;
    if (anchor != no_media_timestamp)
    {
        wall_pts_ = anchor;
        wall_clock_.restart();
    }
    audio_id_ = audio_id;
    audio_period_bytes_ = period_bytes;
    if (audio_id_ != 0)
    {
        SDL_PauseAudioDevice(audio_id_, 1);
        SDL_ClearQueuedAudio(audio_id_);
        emit audible_changed(true);
    }
}

bool playback_view::audible() const { return audio_id_ != 0; }
const QImage& playback_view::image() const { return image_; }
void playback_view::set_keep_aspect(bool keep)
{
    keep_aspect_ = keep;
    update();
}
QString playback_view::status() const { return status_; }
decoder_snapshot playback_view::snapshot() const { return decoder_ ? decoder_->snapshot() : final_stats_; }
quint64 playback_view::rendered_frames() const { return rendered_frames_; }

void playback_view::set_status(const QString& text)
{
    status_ = text;
    emit status_changed(status_);
    update();
}

void playback_view::update_media()
{
    if (!decoder_)
    {
        timer_.stop();
        return;
    }
    auto frames = decoder_->take_video_frames();
    for (auto& frame : frames)
    {
        if (video_.size() >= (audible() ? 16U : 4U))
        {
            video_.pop_front();
        }
        video_.push_back(std::move(frame));
        while (audible() && video_.size() > 1 && video_.front().pts_us != no_media_timestamp && video_.back().pts_us != no_media_timestamp &&
               video_.back().pts_us - video_.front().pts_us > 500'000)
        {
            video_.pop_front();
        }
    }
    auto audio = decoder_->take_audio_blocks();
    if (audible())
    {
        for (auto& block : audio)
        {
            pcm_.push_back(std::move(block));
        }
        qsizetype queued = 0;
        for (const auto& block : pcm_)
        {
            queued += block.pcm.size();
        }
        // 有界保留最近的 PCM；真实 PTS 跳变由下面的连续性检查处理。
        while (queued > 57'600 && pcm_.size() > 1)
        {
            queued -= pcm_.front().pcm.size();
            pcm_.pop_front();
        }
        if (audio_id_ != 0 && SDL_GetAudioDeviceStatus(audio_id_) == SDL_AUDIO_STOPPED)
        {
            set_audible(0, 0);
            set_status(QStringLiteral("音频设备已断开；继续静音解码"));
            emit playback_error(status_);
        }
        while (audio_id_ != 0 && !pcm_.empty() && SDL_GetQueuedAudioSize(audio_id_) < 19'200)
        {
            auto& block = pcm_.front();
            if (audio_end_pts_ != no_media_timestamp && block.pts_us != no_media_timestamp && std::abs(block.pts_us - audio_end_pts_) > 100'000)
            {
                SDL_ClearQueuedAudio(audio_id_);
            }
            if (SDL_QueueAudio(audio_id_, block.pcm.constData(), static_cast<Uint32>(block.pcm.size())) != 0)
            {
                set_audible(0, 0);
                set_status(QStringLiteral("音频输出失败；继续静音解码"));
                emit playback_error(status_);
                break;
            }
            if (block.pts_us != no_media_timestamp)
            {
                audio_end_pts_ = block.pts_us + block.pcm.size() * 1'000'000 / 192'000;
            }
            pcm_.pop_front();
        }
        if (audio_id_ != 0 && SDL_GetAudioDeviceStatus(audio_id_) == SDL_AUDIO_PAUSED && SDL_GetQueuedAudioSize(audio_id_) >= 9'600)
        {
            SDL_PauseAudioDevice(audio_id_, 0);
        }
    }

    // 已声明音轨的有声播放先建立音频时钟，避免首帧按视频时钟抢跑后再回跳。
    if (audible() && !decoder_->snapshot().audio_codec.isEmpty() && SDL_GetAudioDeviceStatus(audio_id_) != SDL_AUDIO_PLAYING)
    {
        return;
    }
    qint64 clock = no_media_timestamp;
    if (audio_id_ != 0 && audio_end_pts_ != no_media_timestamp && SDL_GetAudioDeviceStatus(audio_id_) == SDL_AUDIO_PLAYING &&
        SDL_GetQueuedAudioSize(audio_id_) != 0)
    {
        // SDL 队列不包含已交给后端的 period，按双 period 估算；同步偏差另用数字回环验证。
        clock = audio_end_pts_ - (SDL_GetQueuedAudioSize(audio_id_) + 2LL * audio_period_bytes_) * 1'000'000 / 192'000;
        wall_pts_ = clock;
        wall_clock_.restart();
    }
    else if (wall_pts_ != no_media_timestamp)
    {
        clock = wall_pts_ + wall_clock_.nsecsElapsed() / 1000;
    }
    if (!video_.empty() && clock == no_media_timestamp)
    {
        wall_pts_ = video_.front().pts_us;
        wall_clock_.start();
        clock = wall_pts_;
    }
    while (!video_.empty())
    {
        if (video_.front().pts_us != no_media_timestamp && clock != no_media_timestamp && video_.front().pts_us > clock + 15'000)
        {
            break;
        }
        auto frame = std::move(video_.front());
        video_.pop_front();
        if (!video_.empty() && frame.pts_us != no_media_timestamp && clock != no_media_timestamp && frame.pts_us < clock - 100'000)
        {
            continue;
        }
        shown_pts_ = frame.pts_us;
        image_ = std::move(frame.image);
        ++rendered_frames_;
        set_status(QStringLiteral("实时预览 · 已解码 %1 帧 / 显示 %2 帧").arg(decoder_->snapshot().video_frames).arg(rendered_frames_));
    }
    if (decoder_->isFinished())
    {
        final_stats_ = decoder_->snapshot();
        decoder_.clear();
        timer_.stop();
        reset_audio();
        set_status(final_stats_.error.isEmpty() ? QStringLiteral("媒体源已结束") : final_stats_.error + QStringLiteral("；请手动重试"));
    }
}

void playback_view::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.fillRect(rect(), QColor(23, 27, 31));
    if (!image_.isNull())
    {
        QSize size = image_.size();
        size.scale(this->size(), keep_aspect_ ? Qt::KeepAspectRatio : Qt::IgnoreAspectRatio);
        painter.drawImage(QRect(QPoint((width() - size.width()) / 2, (height() - size.height()) / 2), size), image_);
    }
    painter.setPen(QColor(226, 231, 235));
    painter.fillRect(QRect(0, height() - 30, width(), 30), QColor(0, 0, 0, 160));
    painter.drawText(rect().adjusted(10, 0, -8, -5), Qt::AlignBottom | Qt::AlignLeft, status_);
    if (property("selected").toBool())
    {
        painter.setPen(QPen(QColor(34, 146, 220), 2));
        painter.drawRect(rect().adjusted(1, 1, -1, -1));
    }
}

void playback_view::mousePressEvent(QMouseEvent* event)
{
    emit selected();
    QWidget::mousePressEvent(event);
}

void playback_view::resizeEvent(QResizeEvent* event)
{
    if (decoder_)
    {
        decoder_->set_output_size(event->size());
    }
    QWidget::resizeEvent(event);
}

void playback_view::dragEnterEvent(QDragEnterEvent* event)
{
    if (event->mimeData()->hasFormat(QStringLiteral("application/x-qabstractitemmodeldatalist")))
    {
        event->acceptProposedAction();
    }
}

void playback_view::dropEvent(QDropEvent* event)
{
    auto bytes = event->mimeData()->data(QStringLiteral("application/x-qabstractitemmodeldatalist"));
    QDataStream stream(&bytes, QIODevice::ReadOnly);
    int row = 0;
    int column = 0;
    QMap<int, QVariant> roles;
    stream >> row >> column >> roles;
    if (stream.status() != QDataStream::Ok || !stream.atEnd() || column != 0)
    {
        return;
    }
    const auto data = roles.value(Qt::UserRole).toMap();
    const auto kind = data.value(QStringLiteral("kind")).toString();
    if ((kind != QStringLiteral("gb_channel") && kind != QStringLiteral("source") && kind != QStringLiteral("push_device")) ||
        data.value(QStringLiteral("id")).toString().isEmpty() ||
        (kind == QStringLiteral("gb_channel") && data.value(QStringLiteral("channel_id")).toString().isEmpty()))
    {
        return;
    }
    emit dropped(data);
    event->acceptProposedAction();
}

}    // namespace media_server
