#ifndef DESKTOP_DECODE_THREAD_H
#define DESKTOP_DECODE_THREAD_H

#include <limits>
#include <vector>

#include <QUrl>
#include <QSize>
#include <QImage>
#include <QMutex>
#include <QThread>
#include <QString>
#include <QByteArray>

namespace media_server
{

inline constexpr qint64 no_media_timestamp = std::numeric_limits<qint64>::min();

struct decoded_video_frame
{
    QImage image;
    qint64 pts_us{no_media_timestamp};
    qint64 duration_us{};
};

struct decoded_audio_block
{
    QByteArray pcm;
    qint64 pts_us{no_media_timestamp};
};

struct decoder_snapshot
{
    quint64 video_frames{};
    quint64 audio_frames{};
    quint64 audio_samples{};
    quint64 pcm_samples{};
    quint64 video_dropped{};
    quint64 audio_dropped{};
    qint64 first_video_pts_us{no_media_timestamp};
    qint64 last_video_pts_us{no_media_timestamp};
    qint64 first_audio_pts_us{no_media_timestamp};
    qint64 last_audio_pts_us{no_media_timestamp};
    int video_width{};
    int video_height{};
    double video_fps{};
    int audio_sample_rate{};
    int audio_channels{};
    QString video_codec;
    QString audio_codec;
    QString error;
    bool ended{};
};

class decode_thread final : public QThread
{
    Q_OBJECT

   public:
    decode_thread(QUrl url, QSize output_size, QObject* parent = nullptr);

   public:
    std::vector<decoded_video_frame> take_video_frames();
    std::vector<decoded_audio_block> take_audio_blocks();
    decoder_snapshot snapshot() const;
    void set_output_size(QSize size);

   protected:
    void run() override;

   private:
    static int interrupt(void* opaque);

   private:
    QUrl url_;
    QSize output_size_;
    qint64 io_deadline_us_{};
    mutable QMutex mutex_;
    std::vector<decoded_video_frame> video_frames_;
    std::vector<decoded_audio_block> audio_blocks_;
    qsizetype pcm_bytes_{};
    decoder_snapshot stats_;
};

}    // namespace media_server

#endif
