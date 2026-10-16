#include <utility>

#include <QUrl>
#include <SDL.h>
#include <QList>
#include <QTest>
#include <QEvent>
#include <QTimer>
#include <QDebug>
#include <QProcess>
#include <QtEndian>
#include <QPointer>
#include <QByteArray>
#include <QTcpServer>
#include <QTcpSocket>
#include <QScopeGuard>
#include <QJsonObject>
#include <QHostAddress>
#include <QJsonDocument>
#include <QElapsedTimer>
#include <QStandardPaths>
#include <QCoreApplication>

#include "desktop/decode_thread.h"
#include "desktop/playback_view.h"
#include "desktop/platform_client.h"

namespace media_server
{

namespace
{

// 仅为本用例提供单视频 RTSP/TCP；不实现服务端媒体、授权或重试逻辑。
class live_fixture final : public QObject
{
   public:
    explicit live_fixture(QList<QByteArray> nals) : nals_(std::move(nals))
    {
        timer_.setTimerType(Qt::PreciseTimer);
        timer_.setInterval(40);
        connect(&timer_, &QTimer::timeout, this, [this]() { send_frame(); });
        connect(&api_,
                &QTcpServer::newConnection,
                this,
                [this]()
                {
                    auto* socket = api_.nextPendingConnection();
                    connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                    connect(socket,
                            &QTcpSocket::readyRead,
                            this,
                            [this, socket]()
                            {
                                auto bytes = socket->property("request").toByteArray() + socket->readAll();
                                socket->setProperty("request", bytes);
                                const auto end = bytes.indexOf("\r\n\r\n");
                                if (end < 0 || socket->property("replied").toBool())
                                {
                                    return;
                                }
                                qint64 length = 0;
                                for (const auto& line : bytes.first(end).split('\n'))
                                {
                                    if (line.toLower().startsWith("content-length:"))
                                    {
                                        bool valid = false;
                                        length = line.mid(15).trimmed().toLongLong(&valid);
                                        if (!valid || length < 0 || length > 65536)
                                        {
                                            error = QStringLiteral("平台请求长度无效");
                                            return;
                                        }
                                    }
                                }
                                if (bytes.size() < end + 4 + length)
                                {
                                    return;
                                }
                                if (!bytes.startsWith("POST /api/play HTTP/1.1\r\n"))
                                {
                                    error = QStringLiteral("收到非预期的平台请求");
                                    return;
                                }
                                socket->setProperty("replied", true);
                                const auto body = QJsonDocument(QJsonObject{{"rtsp_url", media_url()}}).toJson(QJsonDocument::Compact);
                                socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                                              QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                                socket->disconnectFromHost();
                            });
                });
        connect(&rtsp_,
                &QTcpServer::newConnection,
                this,
                [this]()
                {
                    if (peer_)
                    {
                        error = QStringLiteral("收到非预期的第二个 RTSP 客户端");
                        rtsp_.nextPendingConnection()->deleteLater();
                        return;
                    }
                    peer_ = rtsp_.nextPendingConnection();
                    connect(peer_, &QTcpSocket::disconnected, &timer_, &QTimer::stop);
                    connect(peer_, &QTcpSocket::disconnected, peer_, &QObject::deleteLater);
                    connect(peer_, &QTcpSocket::readyRead, this, [this]() { read_requests(); });
                });
    }

    bool start() { return api_.listen(QHostAddress::LocalHost, 0) && rtsp_.listen(QHostAddress::LocalHost, 0); }
    QUrl platform_url() const { return QUrl(QStringLiteral("http://127.0.0.1:%1").arg(api_.serverPort())); }
    QString media_url() const { return QStringLiteral("rtsp://127.0.0.1:%1/live").arg(rtsp_.serverPort()); }
    void advance_timestamp(quint32 ticks) { timestamp_ += ticks; }

    QString error;
    quint64 sent_frames{};
    bool played{};

   private:
    void read_requests()
    {
        input_ += peer_->readAll();
        while (!input_.isEmpty())
        {
            // 接受客户端 RTCP，仅跳过完整 interleaved 消息；不把它误当 RTSP。
            if (input_.front() == '$')
            {
                if (input_.size() < 4)
                {
                    return;
                }
                const auto length = qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(input_.constData() + 2));
                if (input_.size() < 4 + length)
                {
                    return;
                }
                input_.remove(0, 4 + length);
                continue;
            }
            const auto end = input_.indexOf("\r\n\r\n");
            if (end < 0)
            {
                return;
            }
            const auto request = input_.first(end);
            input_.remove(0, end + 4);
            const auto lines = request.split('\n');
            QByteArray cseq;
            for (const auto& line : lines)
            {
                if (line.toLower().startsWith("cseq:"))
                {
                    cseq = line.mid(5).trimmed();
                }
            }
            if (cseq.isEmpty())
            {
                error = QStringLiteral("RTSP 请求缺少 CSeq");
                return;
            }
            const auto method = lines.front().split(' ').front();
            QByteArray headers;
            QByteArray body;
            if (method == "OPTIONS")
            {
                headers = "Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN\r\n";
            }
            else if (method == "DESCRIBE")
            {
                QByteArray sps, pps;
                for (const auto& nal : nals_)
                {
                    if ((nal.front() & 31) == 7)
                    {
                        sps = nal;
                    }
                    if ((nal.front() & 31) == 8)
                    {
                        pps = nal;
                    }
                }
                body =
                    "v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=live fixture\r\nc=IN IP4 127.0.0.1\r\nt=0 0\r\n"
                    "a=control:*\r\nm=video 0 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\na=framerate:25\r\n"
                    "a=fmtp:96 packetization-mode=1;profile-level-id=" +
                    sps.mid(1, 3).toHex() + ";sprop-parameter-sets=" + sps.toBase64() + "," + pps.toBase64() + "\r\na=control:trackID=0\r\n";
                headers = "Content-Type: application/sdp\r\nContent-Base: " + media_url().toUtf8() + "/\r\n";
            }
            else if (method == "SETUP")
            {
                if (!request.contains("RTP/AVP/TCP") || !request.contains("interleaved=0-1"))
                {
                    error = QStringLiteral("未使用预期的 RTSP/TCP interleaved 传输");
                    return;
                }
                headers = "Session: fixture\r\nTransport: RTP/AVP/TCP;unicast;interleaved=0-1;ssrc=12345678\r\n";
            }
            else if (method == "PLAY")
            {
                headers = "Session: fixture\r\nRTP-Info: url=" + media_url().toUtf8() + "/trackID=0;seq=1;rtptime=0\r\n";
            }
            else if (method == "TEARDOWN")
            {
                timer_.stop();
                headers = "Session: fixture\r\n";
            }
            else
            {
                error = QStringLiteral("收到非预期的 RTSP 方法：%1").arg(QString::fromLatin1(method));
                return;
            }
            peer_->write("RTSP/1.0 200 OK\r\nCSeq: " + cseq + "\r\n" + headers + "Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" +
                         body);
            if (method == "PLAY")
            {
                played = true;
                timer_.start();
            }
        }
    }

    void send_packet(const QByteArray& payload, bool marker)
    {
        QByteArray packet(12, '\0');
        packet[0] = static_cast<char>(0x80);
        packet[1] = static_cast<char>(96 | (marker ? 0x80 : 0));
        qToBigEndian(sequence_++, reinterpret_cast<uchar*>(packet.data() + 2));
        qToBigEndian(timestamp_, reinterpret_cast<uchar*>(packet.data() + 4));
        qToBigEndian(quint32(0x12345678), reinterpret_cast<uchar*>(packet.data() + 8));
        packet += payload;
        QByteArray interleaved(4, '\0');
        interleaved[0] = '$';
        qToBigEndian(static_cast<quint16>(packet.size()), reinterpret_cast<uchar*>(interleaved.data() + 2));
        interleaved += packet;
        if (!peer_ || peer_->write(interleaved) != interleaved.size())
        {
            error = QStringLiteral("测试 RTP 写入失败");
            timer_.stop();
        }
    }

    void send_frame()
    {
        for (qsizetype index = 0; index < nals_.size(); ++index)
        {
            send_packet(nals_[index], index + 1 == nals_.size());
        }
        timestamp_ += 3600;
        ++sent_frames;
    }

   private:
    QTcpServer api_;
    QTcpServer rtsp_;
    QPointer<QTcpSocket> peer_;
    QByteArray input_;
    QList<QByteArray> nals_;
    QTimer timer_;
    quint16 sequence_{1};
    quint32 timestamp_{};
};

}    // namespace

class live_playback_view_test final : public QObject
{
    Q_OBJECT

   private slots:
    void future_pts_after_muted_queue_trim_data()
    {
        QTest::addColumn<quint32>("advance");
        QTest::addColumn<bool>("audio_selected");
        QTest::newRow("muted-continuous-25fps") << quint32(0) << false;
        QTest::newRow("muted-forward-500ms") << quint32(45000) << false;
        QTest::newRow("selected-no-audio-continuous-25fps") << quint32(0) << true;
        QTest::newRow("selected-no-audio-forward-1000ms") << quint32(90000) << true;
    }

    void future_pts_after_muted_queue_trim()
    {
        QFETCH(quint32, advance);
        QFETCH(bool, audio_selected);
        QVERIFY(SDL_WasInit(0) == 0);
        const bool had_driver = qEnvironmentVariableIsSet("SDL_AUDIODRIVER");
        const auto previous_driver = qgetenv("SDL_AUDIODRIVER");
        const auto restore_audio = qScopeGuard(
            [&]()
            {
                SDL_Quit();
                if (had_driver)
                {
                    qputenv("SDL_AUDIODRIVER", previous_driver);
                }
                else
                {
                    qunsetenv("SDL_AUDIODRIVER");
                }
            });
        QVERIFY(qputenv("SDL_AUDIODRIVER", "dummy"));
        QCOMPARE(SDL_InitSubSystem(SDL_INIT_AUDIO), 0);
        SDL_AudioSpec wanted{};
        wanted.freq = 48000;
        wanted.format = AUDIO_S16SYS;
        wanted.channels = 2;
        wanted.samples = 512;
        SDL_AudioSpec obtained{};
        const auto audio_id = SDL_OpenAudioDevice(nullptr, 0, &wanted, &obtained, 0);
        QVERIFY2(audio_id != 0, SDL_GetError());
        const auto close_audio = qScopeGuard([&]() { SDL_CloseAudioDevice(audio_id); });
        auto ffmpeg = qEnvironmentVariable("FFMPEG_TEST_EXECUTABLE");
        if (ffmpeg.isEmpty())
        {
            ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
        }
        if (ffmpeg.isEmpty())
        {
            QSKIP("FFmpeg CLI 不存在，未执行真实 RTSP/H.264 呈现回归");
        }
        QProcess generator;
        generator.start(ffmpeg,
                        {"-nostdin",  "-hide_banner",
                         "-loglevel", "error",
                         "-f",        "lavfi",
                         "-i",        "color=c=black:size=64x64:rate=25",
                         "-frames:v", "1",
                         "-c:v",      "libx264",
                         "-preset",   "ultrafast",
                         "-tune",     "zerolatency",
                         "-pix_fmt",  "yuv420p",
                         "-f",        "h264",
                         "pipe:1"},
                        QIODevice::ReadOnly);
        QVERIFY2(generator.waitForStarted(1000), qPrintable(generator.errorString()));
        if (!generator.waitForFinished(5000))
        {
            generator.kill();
            QVERIFY(generator.waitForFinished(1000));
            QFAIL("本轮 fixture FFmpeg 超时");
        }
        QCOMPARE(generator.exitStatus(), QProcess::NormalExit);
        QVERIFY2(generator.exitCode() == 0, generator.readAllStandardError().constData());
        const auto bytes = generator.readAllStandardOutput();
        QList<QByteArray> nals;
        qsizetype start = -1;
        for (qsizetype index = 0; index + 2 < bytes.size(); ++index)
        {
            const int prefix = bytes.mid(index, 4) == QByteArray("\0\0\0\1", 4) ? 4 : bytes.mid(index, 3) == QByteArray("\0\0\1", 3) ? 3 : 0;
            if (!prefix)
            {
                continue;
            }
            if (start >= 0 && index > start)
            {
                nals.append(bytes.mid(start, index - start));
            }
            start = index + prefix;
            index += prefix - 1;
        }
        if (start >= 0 && start < bytes.size())
        {
            nals.append(bytes.mid(start));
        }
        bool sps = false, pps = false, idr = false;
        for (const auto& nal : nals)
        {
            QVERIFY(!nal.isEmpty());
            QVERIFY(nal.size() <= 1200);
            sps |= (nal.front() & 31) == 7;
            pps |= (nal.front() & 31) == 8;
            idr |= (nal.front() & 31) == 5;
        }
        QVERIFY(sps && pps && idr);
        live_fixture fixture(std::move(nals));
        QVERIFY(fixture.start());
        platform_client client;
        QVERIFY(client.set_base_url(fixture.platform_url()));
        playback_view view;
        const auto cleanup = qScopeGuard(
            [&]()
            {
                view.stop();
                for (auto* decoder : QCoreApplication::instance()->findChildren<decode_thread*>())
                {
                    decoder->requestInterruption();
                    if (!decoder->wait(4000))
                    {
                        qFatal("实时呈现回归的 decoder 未在取消后退出");
                    }
                }
                QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            });
        view.play(client, {play_target::kind::source, "live", {}, "live"});
        QTRY_VERIFY_WITH_TIMEOUT(fixture.played, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.error.isEmpty() || view.rendered_frames() > 10, 15000);
        QVERIFY2(fixture.error.isEmpty(), qPrintable(fixture.error));
        QVERIFY2(view.snapshot().error.isEmpty(), qPrintable(view.snapshot().error));
        QVERIFY(view.rendered_frames() > 10);
        QVERIFY(!view.audible());
        QVERIFY(view.snapshot().audio_codec.isEmpty());
        if (audio_selected)
        {
            view.set_audible(audio_id, obtained.samples * 4);
        }
        QCOMPARE(view.audible(), audio_selected);
        const auto sent_before = fixture.sent_frames;
        const auto decoded_before = view.snapshot().video_frames;
        const auto pts_before = view.snapshot().last_video_pts_us;
        fixture.advance_timestamp(advance);
        // 先等阶跃后的真实 RTP 被解码并填满 GUI 队列；不能只观察阶跃前遗留帧。
        QTRY_VERIFY_WITH_TIMEOUT(fixture.sent_frames >= sent_before + 15 && view.snapshot().video_frames >= decoded_before + 10, 2000);
        QVERIFY(view.snapshot().last_video_pts_us - pts_before >= 300000 + advance * 1'000'000LL / 90000);
        const auto rendered = view.rendered_frames();
        const auto decoded = view.snapshot().video_frames;
        const auto sent = fixture.sent_frames;
        QElapsedTimer observation;
        observation.start();
        QTRY_VERIFY_WITH_TIMEOUT(fixture.sent_frames >= sent + 30, 2000);
        QVERIFY(observation.elapsed() >= 1000);
        QVERIFY2(fixture.error.isEmpty(), qPrintable(fixture.error));
        QVERIFY2(view.snapshot().error.isEmpty(), qPrintable(view.snapshot().error));
        QVERIFY(view.snapshot().video_frames >= decoded + 20);
        qInfo() << "advance_ticks" << advance << "audio_selected" << audio_selected << "observed_ms" << observation.elapsed() << "decoded"
                << view.snapshot().video_frames - decoded << "rendered" << view.rendered_frames() - rendered;
        QVERIFY2(view.rendered_frames() >= rendered + 20, "PTS 阶跃后解码持续增长，但无音频时钟的实时呈现未恢复");
        view.stop();
        QTRY_VERIFY_WITH_TIMEOUT(QCoreApplication::instance()->findChildren<decode_thread*>().isEmpty(), 4000);
    }
};

}    // namespace media_server

QTEST_MAIN(media_server::live_playback_view_test)
#include "live_playback_view_test.moc"
