#include <QDir>
#include <QTest>
#include <QProcess>
#include <QTcpServer>
#include <QTcpSocket>
#include <QScopeGuard>
#include <QHostAddress>
#include <QTemporaryDir>
#include <QStandardPaths>
#include <QAbstractSocket>

#include "decode_thread.h"

namespace media_server
{

class decode_thread_test final : public QObject
{
    Q_OBJECT

   private slots:
    void initTestCase()
    {
        QVERIFY(directory_.isValid());
        ffmpeg_ = qEnvironmentVariable("FFMPEG_TEST_EXECUTABLE");
        if (ffmpeg_.isEmpty())
        {
            ffmpeg_ = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
        }
        if (ffmpeg_.isEmpty())
        {
            QSKIP("FFmpeg CLI 不存在，未执行真实 fixture 解码验证");
        }
        const auto generate = [this](QStringList arguments)
        {
            QProcess process;
            arguments.prepend(QStringLiteral("error"));
            arguments.prepend(QStringLiteral("-loglevel"));
            arguments.prepend(QStringLiteral("-hide_banner"));
            arguments.prepend(QStringLiteral("-nostdin"));
            process.start(ffmpeg_, arguments, QIODevice::ReadOnly);
            if (!process.waitForStarted(1000))
            {
                qWarning().noquote() << process.errorString();
                return false;
            }
            if (!process.waitForFinished(5000))
            {
                process.kill();
                process.waitForFinished(1000);
                qWarning() << "本轮 fixture FFmpeg 超时";
                return false;
            }
            if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
            {
                qWarning().noquote() << process.readAllStandardError();
                return false;
            }
            return true;
        };
        const auto base = directory_.filePath(QStringLiteral("h264-aac.mp4"));
        QVERIFY(generate({"-f",      "lavfi",     "-i",       "testsrc2=size=320x240:rate=25",
                          "-f",      "lavfi",     "-i",       "sine=frequency=440:sample_rate=48000",
                          "-t",      "1",         "-c:v",     "libx264",
                          "-preset", "ultrafast", "-pix_fmt", "yuv420p",
                          "-c:a",    "aac",       "-y",       base}));
        QVERIFY(generate({"-i", base, "-c:v", "copy", "-c:a", "pcm_alaw", "-ar", "8000", "-ac", "1", "-y", directory_.filePath("h264-pcma.mov")}));
        QVERIFY(generate({"-i", base, "-c:v", "copy", "-an", "-y", directory_.filePath("video-only.mp4")}));
        QVERIFY(generate({"-i", base, "-c:v", "copy", "-c:a", "libopus", "-y", directory_.filePath("unsupported-opus.mkv")}));
    }

    void decode_media_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<QString>("audio_codec");
        QTest::addColumn<int>("sample_rate");
        QTest::newRow("H264-AAC") << QString("h264-aac.mp4") << QString("aac") << 48000;
        QTest::newRow("H264-PCMA") << QString("h264-pcma.mov") << QString("pcm_alaw") << 8000;
        QTest::newRow("H264-no-audio") << QString("video-only.mp4") << QString() << 0;
    }

    void decode_media()
    {
        QFETCH(QString, name);
        QFETCH(QString, audio_codec);
        QFETCH(int, sample_rate);
        decode_thread thread(QUrl::fromLocalFile(directory_.filePath(name)), QSize(640, 360));
        const auto stop = qScopeGuard(
            [&]()
            {
                thread.requestInterruption();
                if (!thread.wait(4000))
                {
                    qFatal("解码线程在取消后没有退出");
                }
            });
        thread.start();
        QTRY_VERIFY_WITH_TIMEOUT(thread.isFinished(), 4000);
        const auto stats = thread.snapshot();
        QVERIFY2(stats.error.isEmpty(), qPrintable(stats.error));
        QVERIFY(stats.ended);
        QCOMPARE(stats.video_frames, quint64(25));
        QCOMPARE(stats.video_codec, QString("h264"));
        QCOMPARE(stats.video_width, 320);
        QCOMPARE(stats.video_height, 240);
        QCOMPARE(stats.video_fps, 25.0);
        QVERIFY(stats.first_video_pts_us != no_media_timestamp);
        QVERIFY(stats.last_video_pts_us > stats.first_video_pts_us);
        QCOMPARE(stats.audio_codec, audio_codec);
        QCOMPARE(stats.audio_sample_rate, sample_rate);
        const auto video = thread.take_video_frames();
        QCOMPARE(video.size(), size_t(4));
        QCOMPARE(stats.video_dropped, quint64(21));
        for (const auto& frame : video)
        {
            QCOMPARE(frame.image.size(), QSize(480, 360));
            QVERIFY(!frame.image.isNull());
        }
        qsizetype pcm_bytes = 0;
        for (const auto& block : thread.take_audio_blocks())
        {
            pcm_bytes += block.pcm.size();
            QCOMPARE(block.pcm.size() % 4, qsizetype(0));
        }
        QVERIFY(pcm_bytes <= 57600);
        if (audio_codec.isEmpty())
        {
            QCOMPARE(stats.audio_frames, quint64(0));
            QCOMPARE(stats.audio_samples, quint64(0));
            QCOMPARE(pcm_bytes, qsizetype(0));
        }
        else
        {
            QVERIFY(stats.audio_frames > 0);
            QVERIFY(stats.audio_samples >= quint64(sample_rate));
            QVERIFY(stats.pcm_samples >= 48000);
            QVERIFY(pcm_bytes > 0);
            QVERIFY(stats.audio_dropped > 0);
            QVERIFY(stats.last_audio_pts_us > stats.first_audio_pts_us);
        }
        QVERIFY(thread.take_video_frames().empty());
        QVERIFY(thread.take_audio_blocks().empty());
    }

    void unsupported_audio()
    {
        decode_thread thread(QUrl::fromLocalFile(directory_.filePath("unsupported-opus.mkv")), QSize(640, 360));
        thread.start();
        const auto stop = qScopeGuard(
            [&]()
            {
                thread.requestInterruption();
                if (!thread.wait(4000))
                {
                    qFatal("解码线程在取消后没有退出");
                }
            });
        QTRY_VERIFY_WITH_TIMEOUT(thread.isFinished(), 4000);
        const auto stats = thread.snapshot();
        QVERIFY(stats.error.contains("opus"));
        QCOMPARE(stats.video_frames, quint64(0));
        QCOMPARE(stats.audio_frames, quint64(0));
    }

    void cancel_immediately()
    {
        decode_thread thread(QUrl::fromLocalFile(directory_.filePath("h264-aac.mp4")), QSize(640, 360));
        const auto stop = qScopeGuard(
            [&]()
            {
                thread.requestInterruption();
                if (!thread.wait(4000))
                {
                    qFatal("立即取消没有结束解码线程");
                }
            });
        thread.start();
        thread.requestInterruption();
        QVERIFY(thread.wait(4000));
        QVERIFY(thread.snapshot().error.isEmpty());
        QVERIFY(thread.snapshot().ended);
    }

    void cancel_during_rtsp_connection()
    {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        decode_thread thread(QUrl(QString("rtsp://127.0.0.1:%1/test/fake-token").arg(server.serverPort())), QSize(640, 360));
        const auto stop = qScopeGuard(
            [&]()
            {
                thread.requestInterruption();
                if (!thread.wait(4000))
                {
                    qFatal("RTSP 建连取消没有解除阻塞");
                }
            });
        thread.start();
        QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections(), 2000);
        auto* peer = server.nextPendingConnection();
        QVERIFY(peer);
        QTRY_VERIFY_WITH_TIMEOUT(peer->bytesAvailable() > 0, 1000);
        QVERIFY(peer->readAll().startsWith("OPTIONS rtsp://"));
        thread.requestInterruption();
        QTRY_VERIFY_WITH_TIMEOUT(thread.isFinished(), 2000);
        QVERIFY(thread.snapshot().error.isEmpty());
        QCOMPARE(thread.snapshot().video_frames, quint64(0));
        QTRY_COMPARE_WITH_TIMEOUT(peer->state(), QAbstractSocket::UnconnectedState, 1000);
    }

    void rtsp_connection_timeout()
    {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        decode_thread thread(QUrl(QString("rtsp://127.0.0.1:%1/test/fake-token").arg(server.serverPort())), QSize(640, 360));
        const auto stop = qScopeGuard(
            [&]()
            {
                thread.requestInterruption();
                if (!thread.wait(4000))
                {
                    qFatal("RTSP 超时后没有结束解码线程");
                }
            });
        thread.start();
        QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections(), 2000);
        auto* peer = server.nextPendingConnection();
        QVERIFY(peer);
        QTRY_VERIFY_WITH_TIMEOUT(peer->bytesAvailable() > 0, 1000);
        QVERIFY(peer->readAll().startsWith("OPTIONS rtsp://"));
        QTRY_VERIFY_WITH_TIMEOUT(thread.isFinished(), 17000);
        QCOMPARE(thread.snapshot().error, QString("连接媒体超时"));
        QCOMPARE(thread.snapshot().video_frames, quint64(0));
        QTRY_COMPARE_WITH_TIMEOUT(peer->state(), QAbstractSocket::UnconnectedState, 1000);
    }

   private:
    QTemporaryDir directory_;
    QString ffmpeg_;
};

}    // namespace media_server

QTEST_GUILESS_MAIN(media_server::decode_thread_test)
#include "decode_thread_test.moc"
