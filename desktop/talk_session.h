#ifndef DESKTOP_TALK_SESSION_H
#define DESKTOP_TALK_SESSION_H

#include <QMutex>
#include <QTimer>
#include <QPointer>
#include <QWebSocket>
#include <QElapsedTimer>

#include "desktop/platform_client.h"

struct AVFrame;
struct AVPacket;
struct AVCodecContext;

namespace media_server
{

class talk_session final : public QObject
{
    Q_OBJECT

   public:
    talk_session(const QUrl& platform, QString device_id, QObject* parent = nullptr);
    ~talk_session() override;
    void start();
    void stop(const QString& error = {});

   signals:
    void transmitting();
    void progress(quint64 packets);
    void finished(const QString& error);

   private:
    static void capture(void* context, unsigned char* bytes, int length);
    void send_audio();
    void close_capture();

   private:
    platform_client client_;
    QString device_id_;
    QString talk_id_;
    QPointer<QNetworkReply> request_;
    QWebSocket socket_;
    QTimer timer_;
    QElapsedTimer audio_progress_;
    quint32 capture_id_{};
    QMutex mutex_;
    QByteArray pcm_;
    AVCodecContext* encoder_{};
    AVFrame* frame_{};
    AVPacket* packet_{};
    qint64 samples_{};
};

}    // namespace media_server

#endif
