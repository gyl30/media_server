#ifndef DESKTOP_PLAYBACK_VIEW_H
#define DESKTOP_PLAYBACK_VIEW_H

#include <deque>

#include <QUrl>
#include <QImage>
#include <QTimer>
#include <QWidget>
#include <QString>
#include <QPointer>
#include <QVariantMap>
#include <QElapsedTimer>
#include <QDeadlineTimer>

#include "desktop/decode_thread.h"
#include "desktop/record_thread.h"

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
    void play_record(platform_client& client, const play_target& target, qint64 start_time, qint64 end_time);
    void set_record_paused(bool paused);
    void start_recording(platform_client& client, const QString& directory);
    void stop_recording();
    bool recording() const;
    bool recording_ready() const;
    bool record_paused() const;
    bool record_control_ready() const;
    void stop();
    void set_audible(quint32 audio_id, int period_bytes);
    bool audible() const;
    void set_keep_aspect(bool keep);
    const QImage& image() const;
    QString status() const;
    decoder_snapshot snapshot() const;
    quint64 rendered_frames() const;

   signals:
    void selected();
    void dropped(const QVariantMap& data);
    void audible_changed(bool audible);
    void playback_error(const QString& error);
    void status_changed(const QString& status);
    void recording_changed();
    void recording_finished(const QString& message);

   protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

   private:
    void update_media();
    void start_decoder(const QString& name,
                       const QUrl& url,
                       playback_mode mode = playback_mode::realtime,
                       QDeadlineTimer finite_deadline = QDeadlineTimer::Forever);
    void wait_for_gb_media(platform_client& client, const play_target& target, const QString& stream_id, const QUrl& url, QDeadlineTimer deadline);
    void reset_audio();
    void set_status(const QString& text);

   private:
    QObject* session_{};
    QPointer<QNetworkReply> request_;
    QPointer<decode_thread> decoder_;
    QPointer<platform_client> record_owner_;
    play_target live_target_;
    QObject* record_session_{};
    QPointer<record_thread> recorder_;
    QString record_id_;
    bool record_paused_{};
    quint32 audio_id_{};
    int audio_period_bytes_{};
    QTimer timer_;
    QElapsedTimer wall_clock_;
    qint64 wall_pts_{no_media_timestamp};
    qint64 audio_end_pts_{no_media_timestamp};
    qint64 shown_pts_{no_media_timestamp};
    qint64 shown_end_pts_{no_media_timestamp};
    QImage image_;
    QString status_;
    decoder_snapshot final_stats_;
    std::deque<decoded_video_frame> video_;
    std::deque<decoded_audio_block> pcm_;
    quint64 rendered_frames_{};
    bool keep_aspect_{true};
};

}    // namespace media_server

#endif
