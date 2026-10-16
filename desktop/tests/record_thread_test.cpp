#include <csignal>
#include <sys/resource.h>

#include <QDir>
#include <QUrl>
#include <QFile>
#include <QTest>
#include <QTimer>
#include <QPointer>
#include <QByteArray>
#include <QTcpServer>
#include <QTcpSocket>
#include <QScopeGuard>
#include <QHostAddress>
#include <QTemporaryDir>
#include <QCryptographicHash>

#include "desktop/decode_thread.h"
#include "desktop/record_thread.h"

extern "C"
{
#include <libavformat/avformat.h>
}

namespace media_server
{

namespace
{

struct decoded_output
{
    QList<QByteArray> video;
    QCryptographicHash pcm{QCryptographicHash::Sha256};
    qsizetype pcm_bytes{};
    QString error;
};

// 用同一 SDK 的有限解码读取完整文件，作为录像内容对照；不是独立编解码正确性证明。
bool decode_file(const QString& path, decoded_output& output)
{
    decode_thread thread(QUrl::fromLocalFile(path), QSize(320, 240), playback_mode::finite);
    const auto consume = [&]()
    {
        for (const auto& frame : thread.take_video_frames())
        {
            output.video.append(QCryptographicHash::hash(
                QByteArray(reinterpret_cast<const char*>(frame.image.constBits()), frame.image.sizeInBytes()), QCryptographicHash::Sha256));
        }
        for (const auto& block : thread.take_audio_blocks())
        {
            output.pcm.addData(block.pcm);
            output.pcm_bytes += block.pcm.size();
        }
    };
    thread.start();
    QElapsedTimer elapsed;
    elapsed.start();
    while (!thread.isFinished() && elapsed.elapsed() < 8000)
    {
        consume();
        QThread::msleep(1);
    }
    if (!thread.isFinished())
    {
        thread.requestInterruption();
        thread.wait();
        output.error = QStringLiteral("decode timeout");
        return false;
    }
    consume();
    output.error = thread.snapshot().error;
    return output.error.isEmpty();
}

struct container_info
{
    qint64 duration_ms{};
    qint64 first_video_ms{-1};
    int streams{};
};

container_info inspect(const QString& path)
{
    container_info info;
    AVFormatContext* format = nullptr;
    const auto name = QFile::encodeName(path);
    if (avformat_open_input(&format, name.constData(), nullptr, nullptr) < 0)
    {
        return info;
    }
    if (avformat_find_stream_info(format, nullptr) >= 0)
    {
        info.streams = static_cast<int>(format->nb_streams);
        info.duration_ms = format->duration == AV_NOPTS_VALUE ? -1 : format->duration / 1000;
        AVPacket* packet = av_packet_alloc();
        while (av_read_frame(format, packet) >= 0)
        {
            const auto* stream = format->streams[packet->stream_index];
            if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && packet->pts != AV_NOPTS_VALUE)
            {
                info.first_video_ms = av_rescale_q(packet->pts, stream->time_base, AVRational{1, 1000});
                av_packet_unref(packet);
                break;
            }
            av_packet_unref(packet);
        }
        av_packet_free(&packet);
    }
    avformat_close_input(&format);
    return info;
}

QByteArray chunk(const QByteArray& payload) { return QByteArray::number(static_cast<qlonglong>(payload.size()), 16) + "\r\n" + payload + "\r\n"; }

const QByteArray stream_header("HTTP/1.1 200 OK\r\nContent-Type: video/mpeg\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n");

// 一条连接的最小 HTTP 媒体服务端：读完请求头后由用例决定分段发送。
class media_server_fixture final : public QObject
{
   public:
    media_server_fixture()
    {
        connect(&server_,
                &QTcpServer::newConnection,
                this,
                [this]()
                {
                    peer = server_.nextPendingConnection();
                    connect(peer, &QTcpSocket::readyRead, this, [this]() { request += peer->readAll(); });
                });
    }
    bool start() { return server_.listen(QHostAddress::LocalHost, 0); }
    QUrl url() const { return QUrl(QStringLiteral("http://127.0.0.1:%1/play/gb28181/id/token.ps").arg(server_.serverPort())); }
    bool ready() const { return peer && request.contains("\r\n\r\n"); }
    bool send(const QByteArray& bytes) { return peer && peer->write(bytes) == bytes.size(); }

    QPointer<QTcpSocket> peer;
    QByteArray request;

   private:
    QTcpServer server_;
};

}    // namespace

class record_thread_test final : public QObject
{
    Q_OBJECT

   private slots:
    void initTestCase()
    {
        folder_ = qEnvironmentVariable("FINITE_PS_TEST_FIXTURE_DIR");
        // RLIMIT_FSIZE 用例要求写失败返回 EFBIG，而不是由默认信号终止进程。
        std::signal(SIGXFSZ, SIG_IGN);
    }

    void cleanup() { QVERIFY(QCoreApplication::instance()->findChildren<record_thread*>().isEmpty()); }

    void finite_file_data()
    {
        QTest::addColumn<QString>("file_name");
        QTest::addColumn<qint64>("duration_ms");
        QTest::newRow("pcma-2s") << QString("pcma-video-first-end.ps") << qint64(2000);
        QTest::newRow("aac-2s") << QString("aac-audio-first-noend.ps") << qint64(2000);
        QTest::newRow("pcma-8s") << QString("pcma-long-noend.ps") << qint64(8000);
    }

    // 完整输入复制为 Matroska 后逐帧与 PCM 和源一致，时间轴从 0 开始且时长正确。
    void finite_file()
    {
        if (folder_.isEmpty())
        {
            QSKIP("未指定私有PS素材");
        }
        QFETCH(QString, file_name);
        QFETCH(qint64, duration_ms);
        const auto source = QDir(folder_).filePath(file_name);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto target = directory.filePath(QStringLiteral("record.mkv"));
        record_thread thread(QUrl::fromLocalFile(source), target, playback_mode::finite);
        thread.start();
        QVERIFY(thread.wait(10000));
        const auto stats = thread.snapshot();
        QVERIFY2(stats.saved && stats.error.isEmpty(), qPrintable(stats.error));
        QVERIFY(!stats.interrupted);
        QVERIFY(QFile::exists(target));
        QVERIFY(!QFile::exists(target + QStringLiteral(".part")));
        QCOMPARE(stats.bytes, QFile(target).size());
        decoded_output expected;
        decoded_output actual;
        QVERIFY2(decode_file(source, expected), qPrintable(expected.error));
        QVERIFY2(decode_file(target, actual), qPrintable(actual.error));
        QCOMPARE(actual.video.size(), expected.video.size());
        QCOMPARE(actual.video, expected.video);
        QCOMPARE(actual.pcm_bytes, expected.pcm_bytes);
        QCOMPARE(actual.pcm.result(), expected.pcm.result());
        const auto info = inspect(target);
        QCOMPARE(info.streams, 2);
        QVERIFY2(info.first_video_ms >= 0 && info.first_video_ms <= 50, qPrintable(QString::number(info.first_video_ms)));
        QVERIFY2(std::abs(info.duration_ms - duration_ms) <= 100, qPrintable(QString::number(info.duration_ms)));
        qInfo().noquote() << file_name << "mkv bytes=" << stats.bytes << "frames=" << actual.video.size() << "duration_ms=" << info.duration_ms;
    }

    void finite_http_data()
    {
        QTest::addColumn<QString>("scenario");
        QTest::newRow("complete") << QString("complete");
        QTest::newRow("complete-body-delayed-chunklast") << QString("delayed");
        QTest::newRow("missing-chunklast-budget") << QString("budget");
        QTest::newRow("missing-chunklast") << QString("missing");
        QTest::newRow("cancel") << QString("cancel");
        QTest::newRow("complete-chunked-truncated-pes") << QString("truncated");
    }

    // 只有 HTTP chunked 完整结束才保存；缺终止块或取消都不留下目标或临时文件。
    void finite_http()
    {
        if (folder_.isEmpty())
        {
            QSKIP("未指定私有PS素材");
        }
        QFETCH(QString, scenario);
        QFile file(QDir(folder_).filePath(QStringLiteral("pcma-long-noend.ps")));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto payload = file.readAll();
        QTemporaryDir directory;
        const auto target = directory.filePath(QStringLiteral("download.mkv"));
        media_server_fixture media;
        QVERIFY(media.start());
        record_thread thread(media.url(), target, playback_mode::finite, QDeadlineTimer(scenario == "budget" ? 1500 : 30000, Qt::PreciseTimer));
        const auto stop = qScopeGuard(
            [&]()
            {
                thread.requestInterruption();
                thread.wait();
            });
        thread.start();
        QTRY_VERIFY_WITH_TIMEOUT(media.ready(), 3000);
        QVERIFY(media.request.startsWith("GET /play/gb28181/id/token.ps HTTP/1.1\r\n"));
        QTimer tail;
        if (scenario == "cancel")
        {
            QVERIFY(media.send(stream_header + chunk(payload.first(payload.size() * 7 / 10))));
            QTRY_VERIFY_WITH_TIMEOUT(thread.snapshot().video_packets > 10, 5000);
            QVERIFY(QFile::exists(target + QStringLiteral(".part")));
            thread.requestInterruption();
        }
        else
        {
            // 截断场景 HTTP 分帧完整，但最后一个 PES 少 100 字节，解复用器会标记损坏包。
            const auto body = scenario == "truncated" ? payload.first(payload.size() - 100) : payload;
            const auto complete_now = scenario != "missing" && scenario != "delayed" && scenario != "budget";
            QVERIFY(media.send(stream_header + chunk(body) + (complete_now ? QByteArray("0\r\n\r\n") : QByteArray{})));
            if (scenario == "delayed")
            {
                // 探测已完成且封装确实开始；不能把 11 秒全部算在初始探测窗口。
                QTRY_VERIFY_WITH_TIMEOUT(thread.snapshot().video_packets > 10, 5000);
                tail.setSingleShot(true);
                connect(&tail,
                        &QTimer::timeout,
                        this,
                        [&]()
                        {
                            QVERIFY(media.send("0\r\n\r\n"));
                            media.peer->disconnectFromHost();
                        });
                tail.start(11000);
            }
            else if (scenario != "budget")
            {
                media.peer->disconnectFromHost();
            }
        }
        QTRY_VERIFY_WITH_TIMEOUT(thread.isFinished(), scenario == "budget" ? 4000 : 17000);
        const auto stats = thread.snapshot();
        QVERIFY(!QFile::exists(target + QStringLiteral(".part")));
        if (scenario == "complete" || scenario == "delayed")
        {
            QVERIFY2(stats.saved && stats.error.isEmpty(), qPrintable(stats.error));
            decoded_output actual;
            QVERIFY2(decode_file(target, actual), qPrintable(actual.error));
            QCOMPARE(actual.video.size(), qsizetype(200));
            QCOMPARE(stats.video_packets, quint64(200));
            if (scenario == "delayed")
            {
                decoded_output expected;
                QVERIFY2(decode_file(file.fileName(), expected), qPrintable(expected.error));
                QCOMPARE(actual.video, expected.video);
                QCOMPARE(actual.pcm_bytes, expected.pcm_bytes);
                QCOMPARE(actual.pcm.result(), expected.pcm.result());
            }
        }
        else
        {
            QVERIFY(!stats.saved);
            QVERIFY(!QFile::exists(target));
            if (scenario == "budget")
            {
                QVERIFY2(stats.error.contains(QStringLiteral("截止时间")), qPrintable(stats.error));
                QVERIFY(stats.video_packets > 10);
            }
            QCOMPARE(stats.error.isEmpty(), scenario == "cancel");
        }
        qInfo().noquote() << scenario << "saved=" << stats.saved << "error=" << stats.error;
    }

    // 探测期之后才出现的音轨不能越界访问映射，也不能静默丢弃；有限下载明确失败且不留文件。
    void late_stream()
    {
        if (folder_.isEmpty())
        {
            QSKIP("未指定私有PS素材");
        }
        QTemporaryDir directory;
        const auto target = directory.filePath(QStringLiteral("late.mkv"));
        record_thread thread(QUrl::fromLocalFile(QDir(folder_).filePath(QStringLiteral("late-mp2-audio.ps"))), target, playback_mode::finite);
        thread.start();
        QVERIFY(thread.wait(10000));
        const auto stats = thread.snapshot();
        QVERIFY2(!stats.saved,
                 qPrintable(QStringLiteral("audio=%1 video=%2 packets=%3/%4")
                                .arg(stats.audio_codec, stats.video_codec)
                                .arg(stats.video_packets)
                                .arg(stats.audio_packets)));
        QVERIFY2(stats.error.contains(QStringLiteral("新轨道")), qPrintable(stats.error));
        QVERIFY(stats.audio_codec.isEmpty());
        QVERIFY(!QFile::exists(target));
        QVERIFY(!QFile::exists(target + QStringLiteral(".part")));
    }

    // 已有目标或同名临时文件都拒绝写入，且不删除、不改写它们。
    void existing_files()
    {
        if (folder_.isEmpty())
        {
            QSKIP("未指定私有PS素材");
        }
        const auto source = QDir(folder_).filePath(QStringLiteral("pcma-video-first-end.ps"));
        QTemporaryDir directory;
        const auto target = directory.filePath(QStringLiteral("exists.mkv"));
        for (const auto& path : {target, target + QStringLiteral(".part")})
        {
            QFile existing(path);
            QVERIFY(existing.open(QIODevice::WriteOnly));
            QCOMPARE(existing.write("keep"), qint64(4));
            existing.close();
            record_thread thread(QUrl::fromLocalFile(source), target, playback_mode::finite);
            thread.start();
            QVERIFY(thread.wait(10000));
            const auto stats = thread.snapshot();
            QVERIFY(!stats.saved && !stats.error.isEmpty());
            QVERIFY(existing.open(QIODevice::ReadOnly));
            QCOMPARE(existing.readAll(), QByteArray("keep"));
            existing.close();
            QVERIFY(existing.remove());
            QVERIFY(!QFile::exists(path == target ? target + QStringLiteral(".part") : target));
        }
    }

    void file_size_limit_data()
    {
        QTest::addColumn<bool>("trailer");
        QTest::newRow("body") << false;
        QTest::newRow("trailer") << true;
    }

    // 写盘失败（RLIMIT_FSIZE 产生 EFBIG）在正文或最终收尾阶段都删除临时文件，不交付坏文件。
    void file_size_limit()
    {
        if (folder_.isEmpty())
        {
            QSKIP("未指定私有PS素材");
        }
        QFETCH(bool, trailer);
        const auto source = QDir(folder_).filePath(QStringLiteral("pcma-long-noend.ps"));
        QTemporaryDir directory;
        rlim_t limit = 64 * 1024;
        if (trailer)
        {
            const auto reference = directory.filePath(QStringLiteral("reference.mkv"));
            record_thread complete(QUrl::fromLocalFile(source), reference, playback_mode::finite);
            complete.start();
            QVERIFY(complete.wait(10000));
            QVERIFY(complete.snapshot().saved);
            // 最后一批 cluster 与索引只在 trailer 中写出，限制为完整大小减一字节即在收尾阶段失败。
            limit = static_cast<rlim_t>(QFile(reference).size() - 1);
        }
        rlimit original{};
        QCOMPARE(getrlimit(RLIMIT_FSIZE, &original), 0);
        rlimit restricted = original;
        restricted.rlim_cur = limit;
        QCOMPARE(setrlimit(RLIMIT_FSIZE, &restricted), 0);
        const auto restore = qScopeGuard([&]() { setrlimit(RLIMIT_FSIZE, &original); });
        const auto target = directory.filePath(QStringLiteral("limited.mkv"));
        record_thread thread(QUrl::fromLocalFile(source), target, playback_mode::finite);
        thread.start();
        QVERIFY(thread.wait(10000));
        const auto stats = thread.snapshot();
        QVERIFY(!stats.saved);
        QVERIFY(!QFile::exists(target));
        QVERIFY(!QFile::exists(target + QStringLiteral(".part")));
        QVERIFY2(stats.error.contains(trailer ? QStringLiteral("收尾") : QStringLiteral("写入录像文件失败")), qPrintable(stats.error));
        qInfo().noquote() << "limit=" << limit << "error=" << stats.error;
    }

    void realtime_end_data()
    {
        QTest::addColumn<bool>("user_stop");
        QTest::newRow("user-stop") << true;
        QTest::newRow("source-end") << false;
    }

    // 实时录制：用户停止或源结束都写完整封装并保存，源结束标记为中断。
    void realtime_end()
    {
        if (folder_.isEmpty())
        {
            QSKIP("未指定私有PS素材");
        }
        QFETCH(bool, user_stop);
        QFile file(QDir(folder_).filePath(QStringLiteral("pcma-long-noend.ps")));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto payload = file.readAll();
        QTemporaryDir directory;
        const auto target = directory.filePath(QStringLiteral("live.mkv"));
        media_server_fixture media;
        QVERIFY(media.start());
        // 实时模式由 FFmpeg 自身的可中断 http 协议读取，代替单元测试中不可得的 RTSP 服务端。
        record_thread thread(media.url(), target, playback_mode::realtime);
        const auto stop = qScopeGuard(
            [&]()
            {
                thread.requestInterruption();
                thread.wait();
            });
        thread.start();
        QTRY_VERIFY_WITH_TIMEOUT(media.ready(), 3000);
        QVERIFY(media.send(stream_header + chunk(payload.first(payload.size() * 7 / 10))));
        QTRY_VERIFY_WITH_TIMEOUT(thread.snapshot().video_packets > 50, 5000);
        if (user_stop)
        {
            thread.finish();
        }
        else
        {
            media.peer->disconnectFromHost();
        }
        QTRY_VERIFY_WITH_TIMEOUT(thread.isFinished(), 12000);
        const auto stats = thread.snapshot();
        QVERIFY2(stats.saved, qPrintable(stats.error));
        QCOMPARE(stats.interrupted, !user_stop);
        QVERIFY(!QFile::exists(target + QStringLiteral(".part")));
        decoded_output actual;
        QVERIFY2(decode_file(target, actual), qPrintable(actual.error));
        QVERIFY(actual.video.size() > 50);
        QCOMPARE(quint64(actual.video.size()), stats.video_packets);
        const auto info = inspect(target);
        QVERIFY2(info.first_video_ms >= 0 && info.first_video_ms <= 50, qPrintable(QString::number(info.first_video_ms)));
        qInfo().noquote() << "realtime user_stop=" << user_stop << "frames=" << actual.video.size() << "error=" << stats.error;
    }

   private:
    QString folder_;
};

}    // namespace media_server

QTEST_MAIN(media_server::record_thread_test)
#include "record_thread_test.moc"
