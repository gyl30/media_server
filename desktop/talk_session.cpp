#include <cstring>
#include <utility>

#include <SDL.h>
#include <QMutexLocker>
#include <QJsonObject>
#include <QRegularExpression>

extern "C"
{
#include <libavcodec/avcodec.h>
}

#include "desktop/talk_session.h"

namespace media_server
{

talk_session::talk_session(const QUrl& platform, QString device_id, QObject* parent)
    : QObject(parent), client_(this), device_id_(std::move(device_id))
{
    client_.set_base_url(platform);
    socket_.setMaxAllowedIncomingFrameSize(1024);
    socket_.setMaxAllowedIncomingMessageSize(1024);
    timer_.setTimerType(Qt::PreciseTimer);
    connect(&timer_, &QTimer::timeout, this, &talk_session::send_audio);
    connect(&socket_,
            &QWebSocket::connected,
            this,
            [this]()
            {
                audio_progress_.start();
                SDL_PauseAudioDevice(capture_id_, 0);
                timer_.start(20);
                emit transmitting();
            });
    connect(&socket_, &QWebSocket::disconnected, this, [this]() { stop(QStringLiteral("音频上传连接已断开")); });
    connect(&socket_,
            QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error),
            this,
            [this](QAbstractSocket::SocketError) { stop(QStringLiteral("音频上传连接失败：%1").arg(socket_.errorString())); });
    connect(&socket_, &QWebSocket::textMessageReceived, this, [this](const QString&) { stop(QStringLiteral("收到非预期的音频响应")); });
    connect(&socket_, &QWebSocket::binaryMessageReceived, this, [this](const QByteArray&) { stop(QStringLiteral("收到非预期的音频响应")); });
}

talk_session::~talk_session()
{
    timer_.stop();
    disconnect(&socket_, nullptr, this, nullptr);
    socket_.abort();
    close_capture();
}

void talk_session::start()
{
    if (!platform_client::valid_base_url(client_.base_url()) || !QRegularExpression(QStringLiteral("^[0-9]{20}$")).match(device_id_).hasMatch())
    {
        emit finished(QStringLiteral("无效的平台或国标设备"));
        return;
    }
    SDL_AudioSpec wanted{};
    wanted.freq = 8000;
    wanted.format = AUDIO_S16SYS;
    wanted.channels = 1;
    wanted.samples = 256;
    wanted.callback = &talk_session::capture;
    wanted.userdata = this;
    SDL_AudioSpec obtained{};
    capture_id_ = SDL_OpenAudioDevice(nullptr, 1, &wanted, &obtained, 0);
    if (capture_id_ == 0)
    {
        emit finished(QStringLiteral("无法打开麦克风：%1").arg(QString::fromUtf8(SDL_GetError())));
        return;
    }
    if (obtained.freq != wanted.freq || obtained.format != wanted.format || obtained.channels != wanted.channels)
    {
        stop(QStringLiteral("麦克风没有提供 8kHz 单声道 PCM"));
        return;
    }
    encoder_ = avcodec_alloc_context3(avcodec_find_encoder(AV_CODEC_ID_PCM_ALAW));
    frame_ = av_frame_alloc();
    packet_ = av_packet_alloc();
    if (!encoder_ || !frame_ || !packet_)
    {
        stop(QStringLiteral("音频编码器分配失败"));
        return;
    }
    encoder_->sample_rate = 8000;
    encoder_->sample_fmt = AV_SAMPLE_FMT_S16;
    encoder_->time_base = {1, 8000};
    encoder_->ch_layout = AV_CHANNEL_LAYOUT_MONO;
    frame_->sample_rate = 8000;
    frame_->format = AV_SAMPLE_FMT_S16;
    frame_->nb_samples = 160;
    frame_->ch_layout = AV_CHANNEL_LAYOUT_MONO;
    if (avcodec_open2(encoder_, nullptr, nullptr) < 0 || av_frame_get_buffer(frame_, 0) < 0)
    {
        stop(QStringLiteral("PCMA 音频编码初始化失败"));
        return;
    }
    request_ = client_.request(
        "POST",
        QStringLiteral("/api/devices/%1/talk").arg(device_id_),
        {},
        this,
        [this](platform_response response)
        {
            request_ = nullptr;
            if (!response.ok())
            {
                close_capture();
                emit finished(QStringLiteral("对讲建立失败：%1").arg(response.error));
                return;
            }
            const auto ticket = response.document.object();
            talk_id_ = ticket.value(QStringLiteral("talk_id")).toString();
            const QUrl upload(ticket.value(QStringLiteral("upload_url")).toString(), QUrl::StrictMode);
            const auto path = QRegularExpression(QStringLiteral("^/publish/gb28181/audio/([0-9a-f]{64})/[0-9a-f]{64}$")).match(upload.path());
            if (!upload.isValid() || upload.isRelative() || upload.host().isEmpty() || upload.port() == 0 ||
                (upload.scheme() != QStringLiteral("ws") && upload.scheme() != QStringLiteral("wss")) ||
                upload.authority(QUrl::FullyEncoded).contains(QLatin1Char('@')) || upload.hasQuery() || upload.hasFragment() || !path.hasMatch() ||
                path.captured(1) != talk_id_)
            {
                stop(QStringLiteral("平台返回无效的音频上传凭据"));
                return;
            }
            if (capture_id_ == 0)
            {
                stop();
                return;
            }
            timer_.start(5000);
            socket_.open(upload);
        },
        15'000);
}

void talk_session::capture(void* context, unsigned char* bytes, int length)
{
    auto* self = static_cast<talk_session*>(context);
    const auto size = static_cast<qsizetype>(length & ~1);
    const QMutexLocker lock(&self->mutex_);
    if (size == 0)
    {
        return;
    }
    self->audio_progress_.restart();
    constexpr qsizetype capacity = 3200;
    if (size >= capacity)
    {
        self->pcm_ = QByteArray(reinterpret_cast<const char*>(bytes) + size - capacity, capacity);
        return;
    }
    if (self->pcm_.size() + size > capacity)
    {
        self->pcm_.remove(0, self->pcm_.size() + size - capacity);
    }
    self->pcm_.append(reinterpret_cast<const char*>(bytes), size);
}

void talk_session::send_audio()
{
    if (socket_.state() != QAbstractSocket::ConnectedState)
    {
        stop(QStringLiteral("音频上传连接超时"));
        return;
    }
    if (SDL_GetAudioDeviceStatus(capture_id_) != SDL_AUDIO_PLAYING)
    {
        stop(QStringLiteral("麦克风没有持续提供音频"));
        return;
    }
    if (socket_.bytesToWrite() > 1660)
    {
        stop(QStringLiteral("音频上传积压，已停止对讲"));
        return;
    }
    if (av_frame_make_writable(frame_) < 0)
    {
        stop(QStringLiteral("音频帧分配失败"));
        return;
    }
    bool expired;
    {
        const QMutexLocker lock(&mutex_);
        expired = audio_progress_.hasExpired(3000);
        if (!expired && pcm_.size() < 320)
        {
            return;
        }
        if (!expired)
        {
            std::memcpy(frame_->data[0], pcm_.constData(), 320);
            pcm_.remove(0, 320);
        }
    }
    if (expired)
    {
        stop(QStringLiteral("麦克风没有持续提供音频"));
        return;
    }
    frame_->pts = samples_;
    if (avcodec_send_frame(encoder_, frame_) < 0 || avcodec_receive_packet(encoder_, packet_) < 0 || packet_->size != 160)
    {
        stop(QStringLiteral("PCMA 音频编码失败"));
        return;
    }
    const QByteArray encoded(reinterpret_cast<const char*>(packet_->data), packet_->size);
    av_packet_unref(packet_);
    const auto sent = socket_.sendBinaryMessage(encoded);
    if (capture_id_ == 0)
    {
        return;
    }
    if (sent != 160)
    {
        stop(QStringLiteral("音频上传写入失败"));
        return;
    }
    samples_ += 160;
    emit progress(static_cast<quint64>(samples_ / 160));
}

void talk_session::close_capture()
{
    if (capture_id_ != 0)
    {
        SDL_CloseAudioDevice(capture_id_);
        capture_id_ = 0;
    }
    avcodec_free_context(&encoder_);
    av_frame_free(&frame_);
    av_packet_free(&packet_);
    const QMutexLocker lock(&mutex_);
    pcm_.clear();
}

void talk_session::stop(const QString& error)
{
    if (capture_id_ == 0 && !request_ && talk_id_.isEmpty())
    {
        return;
    }
    timer_.stop();
    disconnect(&socket_, nullptr, this, nullptr);
    socket_.abort();
    close_capture();
    if (request_)
    {
        return;
    }
    if (talk_id_.isEmpty())
    {
        emit finished(error);
        return;
    }
    const auto id = std::exchange(talk_id_, {});
    request_ = client_.request(
        "DELETE",
        QStringLiteral("/api/devices/%1/talk").arg(device_id_),
        {{QStringLiteral("talk_id"), id}},
        this,
        [this, error](platform_response response)
        {
            request_ = nullptr;
            emit finished(response.ok() || response.status == 404 ? error : error + QStringLiteral("；对讲清理未确认：") + response.error);
        },
        5000);
}

}    // namespace media_server
