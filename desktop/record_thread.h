#ifndef DESKTOP_RECORD_THREAD_H
#define DESKTOP_RECORD_THREAD_H

#include <atomic>

#include <QUrl>
#include <QMutex>
#include <QThread>
#include <QString>

#include "desktop/decode_thread.h"

namespace media_server
{

struct record_snapshot
{
    qint64 bytes{};
    qint64 media_us{};
    quint64 video_packets{};
    quint64 audio_packets{};
    QString video_codec;
    QString audio_codec;
    QString error;
    bool saved{};
    bool interrupted{};
};

class record_thread final : public QThread
{
    Q_OBJECT

   public:
    record_thread(QUrl url, QString target, playback_mode mode, QDeadlineTimer finite_deadline = QDeadlineTimer::Forever, QObject* parent = nullptr);

   public:
    void finish();
    record_snapshot snapshot() const;

   protected:
    void run() override;

   private:
    static int interrupt(void* opaque);

   private:
    QUrl url_;
    QString target_;
    playback_mode mode_;
    QDeadlineTimer finite_deadline_;
    std::atomic<bool> finish_requested_{};
    qint64 io_deadline_us_{};
    mutable QMutex mutex_;
    record_snapshot stats_;
};

}    // namespace media_server

#endif
