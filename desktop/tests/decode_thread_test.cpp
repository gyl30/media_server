#include <QDir>
#include <QTest>
#include <QFile>
#include <QTimer>
#include <QProcess>
#include <QTcpServer>
#include <QTcpSocket>
#include <QByteArray>
#include <QScopeGuard>
#include <QMetaObject>
#include <QHostAddress>
#include <QTemporaryDir>
#include <QStandardPaths>
#include <QAbstractSocket>
#include <QCryptographicHash>

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
        QVERIFY(generate(
            {"-i", base, "-an", "-pix_fmt", "bgra", "-sws_flags", "fast_bilinear", "-f", "rawvideo", "-y", directory_.filePath("video.bgra")}));
        for (const auto& name : {QString("h264-aac.mp4"), QString("h264-pcma.mov")})
        {
            QStringList arguments{"-i", directory_.filePath(name), "-vn"};
            if (name == QStringLiteral("h264-aac.mp4"))
            {
                // 该素材有效时长为 48000 样本；CLI 7.1.1 保留的末帧 128 个 padding 不属于 MP4 有效范围。
                // 锁定 SDK 按容器末包时长裁尾；参考仍严格比较全部 48000 样本的完整 PCM 哈希。
                arguments << "-af" << "atrim=end_sample=48000";
            }
            arguments << "-ar" << "48000" << "-ac" << "2" << "-c:a" << "pcm_s16le" << "-f" << "s16le" << "-y" << directory_.filePath(name + ".pcm");
            QVERIFY(generate(arguments));
        }
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

    void finite_media_data() { decode_media_data(); }

    void finite_media()
    {
        QFETCH(QString, name);
        QFETCH(QString, audio_codec);
        QFETCH(int, sample_rate);
        decode_thread thread(QUrl::fromLocalFile(directory_.filePath(name)), QSize(320, 240), playback_mode::finite);
        const auto stop = qScopeGuard(
            [&]()
            {
                thread.requestInterruption();
                if (!thread.wait(4000))
                {
                    qFatal("有限解码取消没有退出");
                }
            });
        output_capture output;
        QTimer consumer;
        connect(&consumer, &QTimer::timeout, this, [&]() { output.consume(thread); });
        consumer.start(1);
        thread.start();
        QTRY_VERIFY_WITH_TIMEOUT(thread.isFinished(), 4000);
        consumer.stop();
        output.consume(thread);
        const auto stats = thread.snapshot();
        QVERIFY2(stats.error.isEmpty(), qPrintable(stats.error));
        QVERIFY(stats.ended);
        QCOMPARE(thread.mode(), playback_mode::finite);
        QCOMPARE(stats.video_frames, quint64(25));
        QCOMPARE(output.video_frames, stats.video_frames);
        QCOMPARE(stats.video_dropped, quint64(0));
        QCOMPARE(stats.audio_dropped, quint64(0));
        QCOMPARE(stats.audio_codec, audio_codec);
        QCOMPARE(stats.audio_sample_rate, sample_rate);
        QCOMPARE(output.pcm_bytes, qsizetype(stats.pcm_samples * 4));
        QFile video_reference(directory_.filePath("video.bgra"));
        QVERIFY(video_reference.open(QIODevice::ReadOnly));
        const auto video = video_reference.readAll();
        QCOMPARE(output.video_bytes, video.size());
        QCOMPARE(output.video_hash.result(), QCryptographicHash::hash(video, QCryptographicHash::Sha256));
        if (audio_codec.isEmpty())
        {
            QCOMPARE(stats.audio_samples, quint64(0));
            QCOMPARE(output.pcm_bytes, qsizetype(0));
        }
        else
        {
            // 这两种输入均为整数倍率，完整 SWR flush 后不能少尾部样本。
            QCOMPARE(stats.pcm_samples, stats.audio_samples * (48000 / sample_rate));
            QFile pcm_reference(directory_.filePath(name + ".pcm"));
            QVERIFY(pcm_reference.open(QIODevice::ReadOnly));
            const auto pcm = pcm_reference.readAll();
            QCOMPARE(output.pcm_bytes, pcm.size());
            QCOMPARE(output.pcm_hash.result(), QCryptographicHash::hash(pcm, QCryptographicHash::Sha256));
        }
        QVERIFY(thread.take_video_frames().empty());
        QVERIFY(thread.take_audio_blocks().empty());
    }

    void finite_backpressure_cancel_data()
    {
        QTest::addColumn<bool>("pcm_backpressure");
        QTest::newRow("video-queue") << false;
        QTest::newRow("PCM-queue") << true;
    }

    void finite_backpressure_cancel()
    {
        QFETCH(bool, pcm_backpressure);
        const auto name = pcm_backpressure ? "h264-pcma.mov" : "video-only.mp4";
        decode_thread thread(QUrl::fromLocalFile(directory_.filePath(name)), QSize(320, 240), playback_mode::finite);
        const auto stop = qScopeGuard(
            [&]()
            {
                thread.requestInterruption();
                if (!thread.wait(4000))
                {
                    qFatal("有限队列背压取消没有退出");
                }
            });
        QTimer video_consumer;
        if (pcm_backpressure)
        {
            connect(&video_consumer, &QTimer::timeout, this, [&]() { thread.take_video_frames(); });
            video_consumer.start(1);
        }
        thread.start();
        if (pcm_backpressure)
        {
            // 仅持续消费视频；PCM 队列填满后，当前音频块停在分段入队等待点。
            QTRY_VERIFY_WITH_TIMEOUT(thread.snapshot().pcm_samples > quint64(decoded_pcm_capacity / 4), 2000);
        }
        else
        {
            // 计数先于入队递增；第五帧已解码、正在等待四帧队列腾出空间。
            QTRY_COMPARE_WITH_TIMEOUT(thread.snapshot().video_frames, quint64(decoded_video_capacity + 1), 2000);
        }
        QVERIFY(!thread.isFinished());
        QCOMPARE(thread.snapshot().video_dropped, quint64(0));
        thread.requestInterruption();
        QTRY_VERIFY_WITH_TIMEOUT(thread.isFinished(), 2000);
        video_consumer.stop();
        const auto stats = thread.snapshot();
        QVERIFY(stats.error.isEmpty());
        QVERIFY(stats.ended);
        if (pcm_backpressure)
        {
            qsizetype bytes = 0;
            for (const auto& block : thread.take_audio_blocks())
            {
                bytes += block.pcm.size();
            }
            QCOMPARE(bytes, decoded_pcm_capacity);
        }
        else
        {
            QCOMPARE(stats.video_frames, quint64(decoded_video_capacity + 1));
            QCOMPARE(thread.take_video_frames().size(), decoded_video_capacity);
        }
        QCOMPARE(stats.video_dropped, quint64(0));
        QCOMPARE(stats.audio_dropped, quint64(0));
    }

    void finite_ps_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<QString>("audio_codec");
        QTest::addColumn<quint64>("audio_samples");
        QTest::addColumn<quint64>("pcm_samples");
        QTest::newRow("PCMA-audio-first-no-PS-end")
            << QString("pcma-audio-first-noend.ps") << QString("pcm_alaw") << quint64(16000) << quint64(96000);
        QTest::newRow("AAC-video-first-PS-end") << QString("aac-video-first-end.ps") << QString("aac") << quint64(97280) << quint64(97280);
    }

    void finite_ps()
    {
        const auto folder = qEnvironmentVariable("FINITE_PS_TEST_FIXTURE_DIR");
        if (folder.isEmpty())
        {
            QSKIP("未指定私有 PS 素材目录；默认 CTest 不代表 PS 解码验收");
        }
        QFETCH(QString, name);
        QFETCH(QString, audio_codec);
        QFETCH(quint64, audio_samples);
        QFETCH(quint64, pcm_samples);
        QVERIFY(QFile::exists(QDir(folder).filePath(name)));
        decode_thread thread(QUrl::fromLocalFile(QDir(folder).filePath(name)), QSize(320, 240), playback_mode::finite);
        const auto stop = qScopeGuard(
            [&]()
            {
                thread.requestInterruption();
                if (!thread.wait(4000))
                {
                    qFatal("PS 解码取消没有退出");
                }
            });
        output_capture output;
        QTimer consumer;
        connect(&consumer, &QTimer::timeout, this, [&]() { output.consume(thread); });
        consumer.start(1);
        thread.start();
        QTRY_VERIFY_WITH_TIMEOUT(thread.isFinished(), 4000);
        consumer.stop();
        output.consume(thread);
        const auto stats = thread.snapshot();
        QVERIFY2(stats.error.isEmpty(), qPrintable(stats.error));
        QCOMPARE(stats.video_frames, quint64(50));
        QCOMPARE(output.video_frames, quint64(50));
        QCOMPARE(stats.audio_codec, audio_codec);
        QCOMPARE(stats.audio_samples, audio_samples);
        QCOMPARE(stats.pcm_samples, pcm_samples);
        QCOMPARE(output.pcm_bytes, qsizetype(pcm_samples * 4));
        QCOMPARE(stats.video_dropped, quint64(0));
        QCOMPARE(stats.audio_dropped, quint64(0));
        qInfo().noquote() << name << "video-BGRA-SHA256=" << output.video_hash.result().toHex()
                          << "PCM-S16-SHA256=" << output.pcm_hash.result().toHex();
    }

    void finite_http_data()
    {
        QTest::addColumn<QString>("scenario");
        QTest::addColumn<bool>("success");
        QTest::newRow("complete-chunked") << QString("complete") << true;
        QTest::newRow("fragmented-header-and-chunks") << QString("fragmented") << true;
        QTest::newRow("complete-body-delayed-chunklast") << QString("delayed") << true;
        QTest::newRow("short-body-delayed-probe-chunklast") << QString("probe-delayed") << true;
        QTest::newRow("complete-before-budget-backpressure-after-budget") << QString("eof-budget") << true;
        QTest::newRow("missing-chunklast-budget") << QString("budget") << false;
        QTest::newRow("paused-video-backpressure-budget") << QString("paused-video") << false;
        QTest::newRow("paused-audio-backpressure-budget") << QString("paused-audio") << false;
        QTest::newRow("missing-chunklast") << QString("missing") << false;
        QTest::newRow("invalid-chunk-length") << QString("invalid") << false;
        QTest::newRow("declared-chunk-longer-than-FIN") << QString("short") << false;
        QTest::newRow("non-200-status") << QString("status") << false;
        QTest::newRow("wrong-content-type") << QString("type") << false;
        QTest::newRow("bytes-after-chunklast") << QString("extra") << true;
        QTest::newRow("FIN-before-response") << QString("empty") << false;
    }

    void finite_http()
    {
        const auto folder = qEnvironmentVariable("FINITE_PS_TEST_FIXTURE_DIR");
        if (folder.isEmpty())
        {
            QSKIP("未指定私有 PS 素材目录；未执行 PS HTTP 传输解码验证");
        }
        QFETCH(QString, scenario);
        QFETCH(bool, success);
        const bool long_body = scenario == "delayed" || scenario == "budget" || scenario.startsWith("paused-");
        const auto path = QDir(folder).filePath(long_body ? "pcma-long-noend.ps" : "pcma-audio-first-noend.ps");
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto payload = file.readAll();
        QVERIFY(!payload.isEmpty());
        // 同 SDK 本地解码输出作为传输对照，避免把 Qt 图像转换指纹混同于探针的原始 Y 平面指纹。
        decode_thread local(QUrl::fromLocalFile(path), QSize(320, 240), playback_mode::finite);
        const auto local_stop = qScopeGuard(
            [&]()
            {
                local.requestInterruption();
                if (!local.wait(4000))
                {
                    qFatal("HTTP 对照解码没有退出");
                }
            });
        output_capture reference;
        QTimer local_consumer;
        connect(&local_consumer, &QTimer::timeout, this, [&]() { reference.consume(local); });
        local_consumer.start(1);
        local.start();
        QTRY_VERIFY_WITH_TIMEOUT(local.isFinished(), 4000);
        local_consumer.stop();
        reference.consume(local);
        QVERIFY2(local.snapshot().error.isEmpty(), qPrintable(local.snapshot().error));
        QCOMPARE(reference.video_frames, long_body ? quint64(200) : quint64(50));
        QCOMPARE(reference.pcm_bytes, long_body ? qsizetype(384000 * 4) : qsizetype(96000 * 4));

        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        const bool budget = scenario == "budget" || scenario.startsWith("paused-");
        decode_thread thread(QUrl(QString("http://127.0.0.1:%1/history/test.ps").arg(server.serverPort())),
                             QSize(320, 240),
                             playback_mode::finite,
                             QDeadlineTimer(budget || scenario == "eof-budget" ? 1500 : 30000, Qt::PreciseTimer));
        const auto stop = qScopeGuard(
            [&]()
            {
                thread.requestInterruption();
                if (!thread.wait(4000))
                {
                    qFatal("HTTP 有限解码取消没有退出");
                }
            });
        output_capture output;
        QTimer consumer;
        connect(&consumer,
                &QTimer::timeout,
                this,
                [&]()
                {
                    if (scenario == "paused-video")
                    {
                        thread.take_audio_blocks();
                    }
                    else if (scenario == "paused-audio")
                    {
                        thread.take_video_frames();
                    }
                    else
                    {
                        output.consume(thread);
                    }
                });
        if (scenario == "eof-budget")
        {
            QTimer::singleShot(2200, &consumer, [&]() { consumer.start(1); });
        }
        else
        {
            consumer.start(1);
        }
        thread.start();
        QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections(), 2000);
        auto* peer = server.nextPendingConnection();
        QVERIFY(peer);
        QByteArray request;
        QByteArray response = scenario == "status" ? "HTTP/1.1 503 Service Unavailable\r\n" : "HTTP/1.1 200 OK\r\n";
        response += scenario == "type" ? "Content-Type: application/octet-stream\r\n" : "Content-Type: video/mpeg\r\n";
        response += "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n";
        if (scenario == "invalid")
        {
            response += "not-hex\r\n" + payload + "\r\n0\r\n\r\n";
        }
        else if (scenario == "short")
        {
            response += QByteArray::number(static_cast<qlonglong>(payload.size() + 17), 16) + "\r\n" + payload;
        }
        else
        {
            const qsizetype chunk_size = scenario == "fragmented" ? 997 : payload.size();
            for (qsizetype offset = 0; offset < payload.size(); offset += chunk_size)
            {
                const auto chunk = payload.mid(offset, chunk_size);
                response += QByteArray::number(static_cast<qlonglong>(chunk.size()), 16) + "\r\n" + chunk + "\r\n";
            }
            if (scenario != "missing" && scenario != "delayed" && scenario != "probe-delayed" && !budget)
            {
                response += "0\r\n\r\n";
            }
            if (scenario == "extra")
            {
                response += "unexpected trailing bytes";
            }
        }
        if (scenario == "empty")
        {
            response.clear();
        }
        QTimer sender;
        qsizetype sent = 0;
        connect(&sender,
                &QTimer::timeout,
                this,
                [&]()
                {
                    // 首片拆开状态行；后续非整除写入跨 chunk 边界，不用数百次 timer 事件人为限速。
                    const auto part = response.mid(sent, sent == 0 ? 7 : 4093);
                    QCOMPARE(peer->write(part), qint64(part.size()));
                    sent += part.size();
                    if (sent == response.size())
                    {
                        sender.stop();
                        peer->disconnectFromHost();
                    }
                });
        QMetaObject::Connection request_connection;
        const auto respond = [&]()
        {
            request += peer->readAll();
            if (!request.contains("\r\n\r\n"))
            {
                return;
            }
            disconnect(request_connection);
            if (scenario == "fragmented")
            {
                sender.start(1);
            }
            else
            {
                QCOMPARE(peer->write(response), qint64(response.size()));
                if (scenario != "delayed" && scenario != "probe-delayed" && !budget)
                {
                    peer->disconnectFromHost();
                }
            }
        };
        request_connection = connect(peer, &QTcpSocket::readyRead, this, respond);
        respond();
        QTimer tail;
        if (scenario == "delayed" || scenario == "probe-delayed")
        {
            // 先确认完成探测并实际解码，避免延迟仅落在 15 秒探测窗口中。
            if (scenario == "delayed")
            {
                QTRY_VERIFY_WITH_TIMEOUT(thread.snapshot().video_frames > 10, 4000);
            }
            tail.setSingleShot(true);
            connect(&tail,
                    &QTimer::timeout,
                    this,
                    [&]()
                    {
                        QCOMPARE(peer->write("0\r\n\r\n"), qint64(5));
                        peer->disconnectFromHost();
                    });
            // 模拟设备发完全部 PS 后，晚于实时 10 秒期限才关闭完整 HTTP 分帧。
            tail.start(scenario == "delayed" ? 11000 : 16500);
        }
        QTRY_VERIFY_WITH_TIMEOUT(thread.isFinished(), budget ? 4000 : 21000);
        consumer.stop();
        output.consume(thread);
        QVERIFY(request.startsWith("GET /history/test.ps HTTP/1.1\r\n"));
        const auto stats = thread.snapshot();
        QVERIFY(stats.ended);
        if (success)
        {
            QVERIFY2(stats.error.isEmpty(), qPrintable(stats.error));
            QCOMPARE(output.video_frames, reference.video_frames);
            QCOMPARE(output.video_hash.result(), reference.video_hash.result());
            QCOMPARE(output.pcm_bytes, reference.pcm_bytes);
            QCOMPARE(output.pcm_hash.result(), reference.pcm_hash.result());
            QCOMPARE(stats.video_dropped, quint64(0));
            QCOMPARE(stats.audio_dropped, quint64(0));
        }
        else
        {
            QVERIFY2(!stats.error.isEmpty(), "无效 HTTP 响应不能被当作正常媒体 EOF");
            if (budget)
            {
                QVERIFY2(stats.error.contains(QStringLiteral("截止时间")), qPrintable(stats.error));
                QVERIFY(stats.video_frames > 0);
                if (scenario == "paused-video")
                {
                    QCOMPARE(stats.video_frames, quint64(decoded_video_capacity + 1));
                }
                if (scenario == "paused-audio")
                {
                    QVERIFY(stats.pcm_samples * 4 > static_cast<quint64>(decoded_pcm_capacity));
                }
            }
        }
    }

    void output_size_before_start()
    {
        decode_thread thread(QUrl::fromLocalFile(directory_.filePath("video-only.mp4")), QSize(640, 360));
        const auto stop = qScopeGuard(
            [&]()
            {
                thread.requestInterruption();
                if (!thread.wait(4000))
                {
                    qFatal("调整尺寸后解码线程没有退出");
                }
            });
        thread.set_output_size(QSize(128, 128));
        thread.set_output_size(QSize(0, 0));
        thread.start();
        QTRY_VERIFY_WITH_TIMEOUT(thread.isFinished(), 4000);
        QVERIFY2(thread.snapshot().error.isEmpty(), qPrintable(thread.snapshot().error));
        const auto frames = thread.take_video_frames();
        QVERIFY(!frames.empty());
        for (const auto& frame : frames)
        {
            QCOMPARE(frame.image.size(), QSize(128, 96));
        }
    }

    void output_size_during_decode()
    {
        decode_thread thread(QUrl::fromLocalFile(directory_.filePath("h264-aac.mp4")), QSize(128, 128));
        const auto stop = qScopeGuard(
            [&]()
            {
                thread.requestInterruption();
                if (!thread.wait(4000))
                {
                    qFatal("并发调整尺寸后解码线程没有退出");
                }
            });
        QTimer resize;
        int changes = 0;
        connect(&resize, &QTimer::timeout, this, [&]() { thread.set_output_size(++changes % 2 == 0 ? QSize(128, 128) : QSize(640, 360)); });
        resize.start(0);
        thread.start();
        QTRY_VERIFY_WITH_TIMEOUT(thread.isFinished(), 4000);
        resize.stop();
        QVERIFY(changes > 0);
        const auto stats = thread.snapshot();
        QVERIFY2(stats.error.isEmpty(), qPrintable(stats.error));
        QCOMPARE(stats.video_frames, quint64(25));
        const auto frames = thread.take_video_frames();
        QVERIFY(!frames.empty());
        for (const auto& frame : frames)
        {
            QVERIFY(frame.image.size() == QSize(128, 96) || frame.image.size() == QSize(480, 360));
        }
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
    struct output_capture
    {
        QCryptographicHash video_hash{QCryptographicHash::Sha256};
        QCryptographicHash pcm_hash{QCryptographicHash::Sha256};
        quint64 video_frames{};
        qsizetype video_bytes{};
        qsizetype pcm_bytes{};

        void consume(decode_thread& thread)
        {
            const auto frames = thread.take_video_frames();
            QVERIFY(frames.size() <= decoded_video_capacity);
            for (const auto& frame : frames)
            {
                QVERIFY(!frame.image.isNull());
                QCOMPARE(frame.image.size(), QSize(320, 240));
                const QByteArray bytes(reinterpret_cast<const char*>(frame.image.constBits()), frame.image.sizeInBytes());
                video_hash.addData(bytes);
                video_bytes += bytes.size();
                ++video_frames;
            }
            qsizetype batch_bytes = 0;
            for (const auto& block : thread.take_audio_blocks())
            {
                QCOMPARE(block.pcm.size() % 4, qsizetype(0));
                pcm_hash.addData(block.pcm);
                pcm_bytes += block.pcm.size();
                batch_bytes += block.pcm.size();
            }
            QVERIFY(batch_bytes <= decoded_pcm_capacity);
        }
    };

   private:
    QTemporaryDir directory_;
    QString ffmpeg_;
};

}    // namespace media_server

QTEST_GUILESS_MAIN(media_server::decode_thread_test)
#include "decode_thread_test.moc"
