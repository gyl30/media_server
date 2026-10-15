#ifndef DESKTOP_PLAYBACK_VIEW_H
#define DESKTOP_PLAYBACK_VIEW_H

#include <deque>

#include <QUrl>
#include <QImage>
#include <QTimer>
#include <QWidget>
#include <QString>
#include <QPointer>
#include <QElapsedTimer>
#include <QDeadlineTimer>

#include "desktop/decode_thread.h"

class QNetworkReply;

namespace media_server
{

class platform_client;

struct play_target
{
    enum class kind
    {
        gb_channel,
        source,
        push_device
    };
    kind type{kind::gb_channel};
    QString id;
    QString channel_id;
    QString name;
};

class playback_view final : public QWidget
{
    Q_OBJECT

   public:
    explicit playback_view(QWidget* parent = nullptr);
    ~playback_view() override;

   public:
    void play(platform_client& client, const play_target& target);
    void stop();
    void set_audible(bool audible);
    bool audible() const;
    QString status() const;
    decoder_snapshot snapshot() const;
    quint64 rendered_frames() const;

   signals:
    void selected();
    void audible_changed(bool audible);
    void playback_error(const QString& error);
    void status_changed(const QString& status);

   protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

   private:
    void update_media();
    void start_decoder(const QString& name, const QUrl& url);
    void wait_for_gb_media(platform_client& client, const play_target& target, const QString& stream_id, const QUrl& url, QDeadlineTimer deadline);
    void reset_audio();
    void set_status(const QString& text);

   private:
    QObject* session_{};
    QPointer<QNetworkReply> request_;
    QPointer<decode_thread> decoder_;
    quint32 audio_id_{};
    int audio_period_bytes_{};
    QTimer timer_;
    QElapsedTimer wall_clock_;
    qint64 wall_pts_{no_media_timestamp};
    qint64 audio_end_pts_{no_media_timestamp};
    qint64 shown_pts_{no_media_timestamp};
    QImage image_;
    QString status_;
    decoder_snapshot final_stats_;
    std::deque<decoded_video_frame> video_;
    std::deque<decoded_audio_block> pcm_;
    quint64 rendered_frames_{};
    bool audible_{};
};

}    // namespace media_server

#endif
