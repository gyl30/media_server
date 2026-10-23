#include <cmath>
#include <cstdlib>
#include <utility>
#include <algorithm>

#include <SDL.h>
#include <QDir>
#include <QMimeData>
#include <QPainter>
#include <QDateTime>
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
#include "desktop/history_request.h"

extern "C"
{
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}

namespace media_server
{

namespace
{

void play_request(const play_target& target, QString& path, QJsonObject& body)
{
    path = QStringLiteral("/api/play");
    if (target.type == play_target::kind::gb_channel)
    {
        path = QStringLiteral("/api/devices/%1/channels/%2/play")
                   .arg(QString::fromLatin1(QUrl::toPercentEncoding(target.id)), QString::fromLatin1(QUrl::toPercentEncoding(target.channel_id)));
        return;
    }
    const auto key = target.type == play_target::kind::source ? QStringLiteral("source_id") : QStringLiteral("push_device_id");
    body.insert(QStringLiteral("source"), QJsonObject{{key, target.id}});
}

}    // namespace

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
    live_target_ = target;
    set_status(QStringLiteral("%1 · 正在申请播放").arg(target.name));
    QString path;
    QJsonObject body;
    play_request(target, path, body);
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

void playback_view::play_record(platform_client& client, const play_target& target, qint64 start_time, qint64 end_time)
{
    stop();
    final_stats_ = {};
    if (target.type != play_target::kind::gb_channel || target.id.isEmpty() || target.channel_id.isEmpty() || start_time <= 0 ||
        end_time <= start_time)
    {
        set_status(QStringLiteral("录像回放参数无效"));
        return;
    }
    session_ = new QObject(this);
    record_start_ = start_time;
    record_end_ = end_time;
    const auto path =
        QStringLiteral("/api/devices/%1/channels/%2/playback")
            .arg(QString::fromLatin1(QUrl::toPercentEncoding(target.id)), QString::fromLatin1(QUrl::toPercentEncoding(target.channel_id)));
    set_status(QStringLiteral("%1 · 正在申请录像回放").arg(target.name));
    request_history(
        client,
        path,
        {{QStringLiteral("start_time"), start_time}, {QStringLiteral("end_time"), end_time}},
        session_,
        [this, name = target.name](const QUrl& url, platform_client* owner, const QString& id, QDeadlineTimer deadline)
        {
            record_owner_ = owner;
            record_id_ = id;
            start_decoder(name, url, playback_mode::finite, deadline);
        },
        [this](const platform_response& response)
        {
            set_status(response.ok() ? QStringLiteral("平台返回的录像实例或媒体地址无效")
                                     : QStringLiteral("录像回放申请失败：%1").arg(response.error));
        });
}

void playback_view::set_record_paused(bool paused)
{
    if (!record_control_ready() || record_paused_ == paused)
    {
        return;
    }
    request_ = record_owner_->request(
        "POST",
        QStringLiteral("/api/playbacks/%1/%2")
            .arg(QString::fromLatin1(QUrl::toPercentEncoding(record_id_)), paused ? QStringLiteral("pause") : QStringLiteral("resume")),
        {},
        session_,
        [this, paused](platform_response response)
        {
            request_.clear();
            if (!response.ok())
            {
                const auto code = response.document.object().value(QStringLiteral("error")).toString();
                if (!paused && (response.status == 404 || code == QStringLiteral("playback_not_streaming")))
                {
                    stop();
                    set_status(QStringLiteral("录像实例已结束或到期，已停止本次回放"));
                    return;
                }
                set_status(paused ? QStringLiteral("暂停未确认：%1；继续播放").arg(response.error)
                                  : QStringLiteral("继续未确认：%1；本地仍暂停，可重试或停止").arg(response.error));
                emit playback_error(status_);
                return;
            }
            if (paused && presentation_rate_)
            {
                audio_clock();
            }
            record_paused_ = paused;
            if (paused)
            {
                if (wall_pts_ != no_media_timestamp)
                {
                    wall_pts_ += static_cast<qint64>(wall_clock_.nsecsElapsed() / 1000 * presentation_rate_.value_or(1.0));
                }
                if (audio_id_ != 0)
                {
                    SDL_PauseAudioDevice(audio_id_, 1);
                }
                set_status(QStringLiteral("录像已暂停；暂停占用本次回放期限，到期后实例由平台回收"));
                return;
            }
            wall_clock_.restart();
            audio_clock_timer_.restart();
            set_status(QStringLiteral("录像继续回放"));
        },
        8'000);
    set_status(paused ? QStringLiteral("正在请求暂停录像") : QStringLiteral("正在请求继续录像"));
}

bool playback_view::record_paused() const { return record_paused_; }

bool playback_view::record_control_ready() const { return record_owner_ && decoder_ && !request_; }

bool playback_view::record_seek_ready() const
{
    return record_control_ready() && !record_paused_ && !decoder_->isFinished() && rendered_frames_ != 0;
}

qint64 playback_view::record_start() const { return record_start_; }
qint64 playback_view::record_end() const { return record_end_; }

void playback_view::seek_record(double position)
{
    if (!record_seek_ready() || !std::isfinite(position) || position < 0 || position > static_cast<double>(record_end_ - record_start_))
    {
        return;
    }
    request_ = record_owner_->request(
        "POST",
        QStringLiteral("/api/playbacks/%1/seek").arg(QString::fromLatin1(QUrl::toPercentEncoding(record_id_))),
        {{QStringLiteral("position"), position}},
        session_,
        [this](platform_response response)
        {
            request_.clear();
            if (!response.ok())
            {
                if (response.status == 404 || response.document.object().value(QStringLiteral("error")) == QStringLiteral("playback_not_streaming"))
                {
                    stop();
                    set_status(QStringLiteral("录像实例已结束或到期，已停止本次回放"));
                    return;
                }
                set_status(QStringLiteral("定位未确认：%1；不自动重试，请观察画面或停止").arg(response.error));
                emit playback_error(status_);
                return;
            }
            set_status(QStringLiteral("设备已受理定位请求，实际精度由设备决定"));
        },
        8'000);
    set_status(QStringLiteral("正在请求录像定位"));
}

void playback_view::scale_record(double scale)
{
    if (!record_seek_ready() || (scale != 0.25 && scale != 0.5 && scale != 1 && scale != 2 && scale != 4))
    {
        return;
    }
    if (!presentation_rate_)
    {
        presentation_rate_ = 1.0;
        timer_.setTimerType(Qt::PreciseTimer);
        timer_.setInterval(5);
        if (audio_id_ != 0 && audio_end_pts_ != no_media_timestamp)
        {
            const auto frames = (SDL_GetQueuedAudioSize(audio_id_) + 2LL * audio_period_bytes_) / 4;
            if (frames != 0)
            {
                audio_spans_.push_back({0, frames, audio_end_pts_ - frames * 1'000'000 / 48'000, audio_end_pts_});
                audio_output_frames_ = frames;
                audio_clock_frames_ = 0;
                audio_clock_timer_.start();
            }
        }
        audio_input_end_pts_ = audio_end_pts_;
    }
    request_ = record_owner_->request(
        "POST",
        QStringLiteral("/api/playbacks/%1/scale").arg(QString::fromLatin1(QUrl::toPercentEncoding(record_id_))),
        {{QStringLiteral("scale"), scale}},
        session_,
        [this](platform_response response)
        {
            request_.clear();
            if (!response.ok() || response.status != 204)
            {
                if (response.status == 404 || response.document.object().value(QStringLiteral("error")) == QStringLiteral("playback_not_streaming"))
                {
                    stop();
                    set_status(QStringLiteral("录像实例已结束或到期，已停止本次回放"));
                    return;
                }
                const auto error = response.error.isEmpty() ? QStringLiteral("平台未返回204（HTTP %1）").arg(response.status) : response.error;
                set_status(QStringLiteral("倍速结果未知：%1；不自动重试，请观察画面或停止").arg(error));
                emit playback_error(status_);
                return;
            }
            set_status(QStringLiteral("设备已受理倍速请求，按实际输入节奏呈现"));
        },
        8'000);
    set_status(QStringLiteral("正在请求录像倍速"));
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

void playback_view::start_decoder(const QString& name, const QUrl& url, playback_mode mode, QDeadlineTimer finite_deadline)
{
    auto* decoder =
        new decode_thread(url, QSize(std::max(160, width()), std::max(100, height())), mode, finite_deadline, QCoreApplication::instance());
    decoder_ = decoder;
    connect(decoder,
            &QThread::finished,
            session_,
            [this, decoder]()
            {
                update_media();
                if (decoder->mode() == playback_mode::finite)
                {
                    return;
                }
                final_stats_ = decoder->snapshot();
                decoder_.clear();
                timer_.stop();
                reset_audio();
                set_status(final_stats_.error.isEmpty() ? QStringLiteral("媒体源已结束") : final_stats_.error + QStringLiteral("；请手动重试"));
            });
    if (mode == playback_mode::realtime)
    {
        connect(decoder, &QThread::finished, decoder, &QObject::deleteLater);
    }
    set_status(QStringLiteral("%1 · 正在等待自然关键帧").arg(name));
    decoder->start();
    timer_.start();
}

bool playback_view::recording() const { return recorder_ || record_session_; }

bool playback_view::recording_ready() const
{
    return decoder_ && decoder_->mode() == playback_mode::realtime && !live_target_.id.isEmpty() && !recording();
}

void playback_view::start_recording(platform_client& client, const QString& directory)
{
    if (!recording_ready())
    {
        return;
    }
    auto stem = live_target_.name.isEmpty() ? live_target_.id : live_target_.name;
    for (auto& character : stem)
    {
        if (!character.isLetterOrNumber() && character != QLatin1Char('-'))
        {
            character = QLatin1Char('_');
        }
    }
    const auto target =
        QDir(directory).filePath(stem.left(64) + QDateTime::currentDateTime().toString(QStringLiteral("_yyyyMMdd_HHmmss")) + QStringLiteral(".mkv"));
    record_session_ = new QObject(this);
    QString path;
    QJsonObject body;
    play_request(live_target_, path, body);
    client.request(
        "POST",
        path,
        body,
        record_session_,
        [this, target](platform_response response)
        {
            record_session_->deleteLater();
            record_session_ = nullptr;
            const QUrl url(response.document.object().value(QStringLiteral("rtsp_url")).toString(), QUrl::StrictMode);
            if (!response.ok() || !url.isValid() || url.scheme() != QStringLiteral("rtsp") || url.host().isEmpty())
            {
                emit recording_changed();
                emit recording_finished(response.ok() ? QStringLiteral("录制申请返回的 RTSP 地址无效")
                                                      : QStringLiteral("录制申请失败：%1").arg(response.error));
                return;
            }
            auto* recorder = new record_thread(url, target, playback_mode::realtime, QDeadlineTimer::Forever, QCoreApplication::instance());
            recorder_ = recorder;
            connect(recorder,
                    &QThread::finished,
                    this,
                    [this, recorder, target]()
                    {
                        const auto stats = recorder->snapshot();
                        if (recorder_ == recorder)
                        {
                            recorder_.clear();
                        }
                        emit recording_changed();
                        emit recording_finished(!stats.saved ? QStringLiteral("录制失败：%1；未保存文件").arg(stats.error)
                                                : !stats.interrupted
                                                    ? QStringLiteral("录制已保存：%1").arg(target)
                                                    : QStringLiteral("录制中断（%1），已保存：%2")
                                                          .arg(stats.error.isEmpty() ? QStringLiteral("源已结束") : stats.error, target));
                    });
            connect(recorder, &QThread::finished, recorder, &QObject::deleteLater);
            recorder->start();
            emit recording_changed();
        },
        10'000);
    emit recording_changed();
}

void playback_view::stop_recording()
{
    delete record_session_;
    record_session_ = nullptr;
    if (recorder_)
    {
        recorder_->finish();
        recorder_.clear();
    }
    emit recording_changed();
}

void playback_view::stop()
{
    if (recording())
    {
        stop_recording();
    }
    live_target_ = {};
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
        if (decoder_->mode() == playback_mode::finite)
        {
            connect(decoder_, &QThread::finished, decoder_, &QObject::deleteLater);
            if (decoder_->isFinished())
            {
                decoder_->deleteLater();
            }
        }
        decoder_.clear();
    }
    record_owner_.clear();
    record_id_.clear();
    record_start_ = 0;
    record_end_ = 0;
    record_paused_ = false;
    presentation_rate_.reset();
    timer_.stop();
    timer_.setTimerType(Qt::CoarseTimer);
    timer_.setInterval(10);
    reset_audio();
    video_.clear();
    pcm_.clear();
    image_ = {};
    wall_pts_ = no_media_timestamp;
    shown_pts_ = no_media_timestamp;
    shown_end_pts_ = no_media_timestamp;
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
    audio_input_end_pts_ = no_media_timestamp;
    audio_output_frames_ = 0;
    audio_clock_frames_ = 0;
    audio_clock_timer_.invalidate();
    audio_spans_.clear();
    swr_free(&audio_resampler_);
}

qint64 playback_view::audio_clock()
{
    if (audio_id_ == 0)
    {
        return no_media_timestamp;
    }
    const auto queued = SDL_GetQueuedAudioSize(audio_id_);
    if (!presentation_rate_)
    {
        return audio_end_pts_ == no_media_timestamp ? no_media_timestamp
                                                    : audio_end_pts_ - (queued + 2LL * audio_period_bytes_) * 1'000'000 / 192'000;
    }
    if (!record_paused_)
    {
        if (queued != 0)
        {
            audio_clock_frames_ = std::max(audio_clock_frames_, audio_output_frames_ - (queued + 2LL * audio_period_bytes_) / 4);
            audio_clock_timer_.restart();
        }
        else if (audio_clock_timer_.isValid())
        {
            audio_clock_frames_ = std::min(audio_output_frames_, audio_clock_frames_ + audio_clock_timer_.nsecsElapsed() * 48'000 / 1'000'000'000);
            audio_clock_timer_.restart();
        }
    }
    if (audio_spans_.empty())
    {
        return no_media_timestamp;
    }
    while (audio_spans_.size() > 1 && audio_clock_frames_ >= audio_spans_.front().last_frame)
    {
        audio_spans_.pop_front();
    }
    const auto& span = audio_spans_.front();
    return span.first_pts + (span.last_pts - span.first_pts) * (audio_clock_frames_ - span.first_frame) / (span.last_frame - span.first_frame);
}

void playback_view::set_audible(quint32 audio_id, int period_bytes)
{
    if (audio_id_ == audio_id)
    {
        return;
    }
    auto anchor = audio_id == 0 && !video_.empty() && video_.back().pts_us != no_media_timestamp ? video_.back().pts_us : shown_pts_;
    if (presentation_rate_)
    {
        anchor = wall_pts_ == no_media_timestamp
                     ? shown_pts_
                     : wall_pts_ + (record_paused_ ? 0 : static_cast<qint64>(wall_clock_.nsecsElapsed() / 1000 * *presentation_rate_));
    }
    reset_audio();
    pcm_.clear();
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
    const bool finite = decoder_->mode() == playback_mode::finite;
    const bool finished_before_take = decoder_->isFinished();
    const bool finite_tail_taken = finished_before_take && video_.empty() && pcm_.empty();
    bool video_evicted = false;
    auto frames = !finite || video_.size() + decoded_video_capacity <= 16U ? decoder_->take_video_frames() : std::vector<decoded_video_frame>{};
    for (auto& frame : frames)
    {
        if (!finite && video_.size() >= (audible() ? 16U : 4U))
        {
            video_.pop_front();
            video_evicted = true;
        }
        video_.push_back(std::move(frame));
        while (!finite && audible() && video_.size() > 1 && video_.front().pts_us != no_media_timestamp &&
               video_.back().pts_us != no_media_timestamp && video_.back().pts_us - video_.front().pts_us > 500'000)
        {
            video_.pop_front();
            video_evicted = true;
        }
    }
    const bool finished_audio_taken = finished_before_take && pcm_.empty();
    auto audio = !finite || pcm_.empty() ? decoder_->take_audio_blocks() : std::vector<decoded_audio_block>{};
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
        while (!finite && queued > decoded_pcm_capacity && pcm_.size() > 1)
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
        while (audio_id_ != 0 && !pcm_.empty() && SDL_GetQueuedAudioSize(audio_id_) < (presentation_rate_ ? 1'920U : 19'200U))
        {
            auto& block = pcm_.front();
            const auto prior_end = presentation_rate_ ? audio_input_end_pts_ : audio_end_pts_;
            if (prior_end != no_media_timestamp && block.pts_us != no_media_timestamp && std::abs(block.pts_us - prior_end) > 100'000)
            {
                SDL_ClearQueuedAudio(audio_id_);
                audio_spans_.clear();
                audio_output_frames_ = 0;
                audio_clock_frames_ = 0;
                audio_clock_timer_.restart();
                swr_free(&audio_resampler_);
                audio_end_pts_ = no_media_timestamp;
            }
            const auto input_frames =
                presentation_rate_ ? std::min(block.pcm.size() / 4, static_cast<qsizetype>(480 * *presentation_rate_)) : block.pcm.size() / 4;
            const auto input_start = presentation_rate_ && block.pts_us == no_media_timestamp ? audio_input_end_pts_ : block.pts_us;
            const auto input_end = input_start == no_media_timestamp ? no_media_timestamp : input_start + input_frames * 1'000'000 / 48'000;
            QByteArray output;
            qint64 output_end = input_end;
            if (presentation_rate_)
            {
                int result = 0;
                if (!audio_resampler_)
                {
                    AVChannelLayout layout = AV_CHANNEL_LAYOUT_STEREO;
                    result =
                        swr_alloc_set_opts2(&audio_resampler_, &layout, AV_SAMPLE_FMT_S16, 48'000, &layout, AV_SAMPLE_FMT_S16, 48'000, 0, nullptr);
                    if (result >= 0)
                    {
                        result = av_opt_set_int(audio_resampler_, "flags", SWR_FLAG_RESAMPLE, 0);
                    }
                    if (result >= 0)
                    {
                        result = swr_init(audio_resampler_);
                    }
                }
                if (result >= 0)
                {
                    result = swr_set_compensation(audio_resampler_, static_cast<int>(std::lround(48'000 * (1 - *presentation_rate_))), 48'000);
                }
                if (result >= 0)
                {
                    result = swr_get_out_samples(audio_resampler_, static_cast<int>(input_frames));
                }
                if (result >= 0)
                {
                    output.resize(result * 4);
                    auto* destination = reinterpret_cast<uint8_t*>(output.data());
                    const auto* source = reinterpret_cast<const uint8_t*>(block.pcm.constData());
                    result = swr_convert(audio_resampler_, &destination, result, &source, static_cast<int>(input_frames));
                }
                if (result < 0)
                {
                    set_audible(0, 0);
                    set_status(QStringLiteral("音频变速失败；继续静音解码"));
                    emit playback_error(status_);
                    break;
                }
                output.resize(result * 4);
                audio_input_end_pts_ = input_end;
                if (audio_end_pts_ == no_media_timestamp)
                {
                    audio_end_pts_ = input_start;
                }
                if (input_end != no_media_timestamp)
                {
                    output_end = std::max(audio_end_pts_, input_end - swr_get_delay(audio_resampler_, 1'000'000));
                }
            }
            else
            {
                output = block.pcm;
            }
            if (!output.isEmpty() && SDL_QueueAudio(audio_id_, output.constData(), static_cast<Uint32>(output.size())) != 0)
            {
                set_audible(0, 0);
                set_status(QStringLiteral("音频输出失败；继续静音解码"));
                emit playback_error(status_);
                break;
            }
            if (input_start != no_media_timestamp)
            {
                if (finite && wall_pts_ == no_media_timestamp)
                {
                    wall_pts_ = input_start;
                    wall_clock_.start();
                }
                if (presentation_rate_ && !output.isEmpty())
                {
                    audio_spans_.push_back({audio_output_frames_, audio_output_frames_ + output.size() / 4, audio_end_pts_, output_end});
                }
                if (!output.isEmpty())
                {
                    audio_end_pts_ = output_end;
                }
            }
            if (presentation_rate_)
            {
                audio_output_frames_ += output.size() / 4;
                if (!output.isEmpty() && !audio_clock_timer_.isValid())
                {
                    audio_clock_timer_.start();
                }
            }
            if (input_frames * 4 == block.pcm.size())
            {
                pcm_.pop_front();
            }
            else
            {
                block.pcm.remove(0, input_frames * 4);
                block.pts_us = input_end;
            }
        }
        if (audio_id_ != 0 && audio_resampler_ && finished_audio_taken && pcm_.empty())
        {
            QByteArray tail;
            int result = 0;
            do
            {
                result = swr_set_compensation(audio_resampler_, static_cast<int>(std::lround(48'000 * (1 - *presentation_rate_))), 48'000);
                if (result < 0)
                {
                    break;
                }
                result = swr_get_out_samples(audio_resampler_, 0);
                if (result < 0)
                {
                    break;
                }
                const auto offset = tail.size();
                tail.resize(offset + result * 4);
                auto* destination = reinterpret_cast<uint8_t*>(tail.data() + offset);
                result = swr_convert(audio_resampler_, &destination, result, nullptr, 0);
                if (result >= 0)
                {
                    tail.resize(offset + result * 4);
                }
            } while (result > 0);
            swr_free(&audio_resampler_);
            if (result < 0 || (!tail.isEmpty() && SDL_QueueAudio(audio_id_, tail.constData(), static_cast<Uint32>(tail.size())) != 0))
            {
                set_audible(0, 0);
                set_status(QStringLiteral("音频尾部输出失败；继续静音解码"));
                emit playback_error(status_);
            }
            else if (!tail.isEmpty())
            {
                if (audio_input_end_pts_ != no_media_timestamp)
                {
                    audio_spans_.push_back({audio_output_frames_, audio_output_frames_ + tail.size() / 4, audio_end_pts_, audio_input_end_pts_});
                    audio_end_pts_ = audio_input_end_pts_;
                }
                audio_output_frames_ += tail.size() / 4;
                if (!audio_clock_timer_.isValid())
                {
                    audio_clock_timer_.start();
                }
            }
        }
        if (audio_id_ != 0 && !record_paused_ && SDL_GetAudioDeviceStatus(audio_id_) == SDL_AUDIO_PAUSED &&
            SDL_GetQueuedAudioSize(audio_id_) >= (finite ? 1U : 9'600U))
        {
            SDL_PauseAudioDevice(audio_id_, 0);
        }
    }

    if (record_paused_ && !(decoder_->isFinished() && !decoder_->snapshot().error.isEmpty()))
    {
        return;
    }

    if (!finite && audible() && !decoder_->snapshot().audio_codec.isEmpty() && SDL_GetAudioDeviceStatus(audio_id_) != SDL_AUDIO_PLAYING)
    {
        return;
    }
    qint64 clock = no_media_timestamp;
    if (finite && !video_.empty() && video_.front().pts_us != no_media_timestamp && shown_end_pts_ != no_media_timestamp &&
        std::abs(video_.front().pts_us - shown_end_pts_) > 1'000'000)
    {
        wall_pts_ = video_.front().pts_us;
        wall_clock_.restart();
    }
    const auto video_clock = wall_pts_ == no_media_timestamp
                                 ? no_media_timestamp
                                 : wall_pts_ + static_cast<qint64>(wall_clock_.nsecsElapsed() / 1000 * presentation_rate_.value_or(1.0));
    bool adopted_audio_clock = false;
    const auto current_audio_clock = audible() ? audio_clock() : no_media_timestamp;
    if (audio_id_ != 0 && audio_end_pts_ != no_media_timestamp && SDL_GetAudioDeviceStatus(audio_id_) == SDL_AUDIO_PLAYING &&
        (SDL_GetQueuedAudioSize(audio_id_) != 0 || (presentation_rate_ && audio_clock_frames_ < audio_output_frames_)))
    {
        clock = current_audio_clock;
        if (clock == no_media_timestamp)
        {
            clock = video_clock;
        }
        else if (finite && shown_end_pts_ != no_media_timestamp && video_clock != no_media_timestamp && std::abs(clock - video_clock) > 1'000'000)
        {
            clock = video_clock;
        }
        else
        {
            wall_pts_ = clock;
            wall_clock_.restart();
            adopted_audio_clock = true;
        }
    }
    else if (wall_pts_ != no_media_timestamp)
    {
        clock = video_clock;
    }
    if (!video_.empty() && clock == no_media_timestamp)
    {
        wall_pts_ = video_.front().pts_us;
        wall_clock_.start();
        clock = wall_pts_;
    }
    if (video_evicted && (!audible() || audio_end_pts_ == no_media_timestamp) && !video_.empty() && video_.front().pts_us != no_media_timestamp &&
        clock != no_media_timestamp && video_.front().pts_us > clock + 15'000)
    {
        wall_pts_ = video_.front().pts_us;
        wall_clock_.restart();
        clock = wall_pts_;
    }
    if (presentation_rate_ && !finished_before_take && clock != no_media_timestamp)
    {
        auto frontier = video_.empty() ? shown_end_pts_ : no_media_timestamp;
        for (const auto& frame : video_)
        {
            if (frame.pts_us == no_media_timestamp || (frontier != no_media_timestamp && std::abs(frame.pts_us - frontier) > 1'000'000))
            {
                break;
            }
            frontier = frame.pts_us + (frame.duration_us > 0 ? frame.duration_us : 40'000);
        }
        if (!adopted_audio_clock && frontier != no_media_timestamp && clock > frontier)
        {
            clock = frontier;
            wall_pts_ = clock;
            wall_clock_.restart();
        }
        if (frontier != no_media_timestamp && std::abs(frontier - clock) < 1'000'000)
        {
            const auto audio_lead = adopted_audio_clock && !audio_spans_.empty() ? std::max(qint64(0), audio_spans_.back().last_pts - clock) : 0;
            const auto buffered = (frontier - clock - audio_lead) / 1'000'000.0;
            const auto rate = std::clamp(1 + 16 * (buffered - 0.12), 0.25, 4.0);
            wall_pts_ = clock;
            wall_clock_.restart();
            presentation_rate_ = rate;
        }
    }
    while (!video_.empty())
    {
        if (video_.front().pts_us != no_media_timestamp && clock != no_media_timestamp && video_.front().pts_us > clock + 15'000)
        {
            break;
        }
        auto frame = std::move(video_.front());
        video_.pop_front();
        if (!finite && !video_.empty() && frame.pts_us != no_media_timestamp && clock != no_media_timestamp && frame.pts_us < clock - 100'000)
        {
            continue;
        }
        shown_pts_ = frame.pts_us;
        const auto fps = decoder_->snapshot().video_fps;
        shown_end_pts_ = shown_pts_ == no_media_timestamp
                             ? no_media_timestamp
                             : shown_pts_ + (frame.duration_us > 0 ? frame.duration_us : (fps > 0 ? static_cast<qint64>(1'000'000 / fps) : 40'000));
        image_ = std::move(frame.image);
        ++rendered_frames_;
        set_status((finite ? QStringLiteral("录像回放 · 已解码 %1 帧 / 显示 %2 帧") : QStringLiteral("实时预览 · 已解码 %1 帧 / 显示 %2 帧"))
                       .arg(decoder_->snapshot().video_frames)
                       .arg(rendered_frames_));
        if (finite)
        {
            break;
        }
    }
    if (decoder_->isFinished())
    {
        const auto stats = decoder_->snapshot();
        if (finite && stats.error.isEmpty() && finite_tail_taken && video_.empty() && pcm_.empty() && shown_end_pts_ != no_media_timestamp &&
            clock >= shown_end_pts_ && audible() && audio_end_pts_ != no_media_timestamp && std::abs(audio_end_pts_ - clock) > 1'000'000)
        {
            shown_end_pts_ = no_media_timestamp;
            clock = audio_clock();
            wall_pts_ = clock;
            wall_clock_.restart();
        }
        if (finite && stats.error.isEmpty() &&
            (!finite_tail_taken || !video_.empty() || !pcm_.empty() || (shown_end_pts_ != no_media_timestamp && clock < shown_end_pts_) ||
             (audible() && (SDL_GetQueuedAudioSize(audio_id_) != 0 || audio_clock_frames_ < audio_output_frames_ ||
                            (audio_end_pts_ != no_media_timestamp && clock < audio_end_pts_)))))
        {
            return;
        }
        final_stats_ = decoder_->snapshot();
        if (finite)
        {
            decoder_->deleteLater();
            delete session_;
            session_ = nullptr;
        }
        decoder_.clear();
        timer_.stop();
        reset_audio();
        video_.clear();
        pcm_.clear();
        set_status(final_stats_.error.isEmpty()
                       ? (finite ? QStringLiteral("录像传输及呈现已结束；不代表请求范围库存完整") : QStringLiteral("媒体源已结束"))
                       : final_stats_.error + QStringLiteral("；请手动重试"));
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
