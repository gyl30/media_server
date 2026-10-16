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
    // 实时录制因源结束或网络中断而保存，不是用户停止。
    bool interrupted{};
};

// 把实时 RTSP 或有限 PS 输入流复制为 Matroska，不解码。先写本次新建的 <目标>.part，
// 收尾全部成功后改名为目标；取消、写盘失败或有限输入不完整时删除 .part。
class record_thread final : public QThread
{
    Q_OBJECT

   public:
    record_thread(QUrl url, QString target, playback_mode mode, QObject* parent = nullptr);

   public:
    // 实时录制停止并保存；有限下载只能取消（requestInterruption）。
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
    std::atomic<bool> finish_requested_{};
    qint64 io_deadline_us_{};
    mutable QMutex mutex_;
    record_snapshot stats_;
};

}    // namespace media_server

#endif
