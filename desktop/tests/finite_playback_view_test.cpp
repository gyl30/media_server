#include <memory>
#include <limits>
#include <algorithm>

#include <QDir>
#include <QUrl>
#include <SDL.h>
#include <QFile>
#include <QList>
#include <QTest>
#include <QEvent>
#include <QTimer>
#include <QPointer>
#include <QByteArray>
#include <QTcpServer>
#include <QTcpSocket>
#include <QSignalSpy>
#include <QScopeGuard>
#include <QJsonObject>
#include <QHostAddress>
#include <QJsonDocument>
#include <QDateTime>
#include <QElapsedTimer>
#include <QCoreApplication>
#include <QCryptographicHash>

#include "desktop/decode_thread.h"
#include "desktop/playback_view.h"
#include "desktop/platform_client.h"
#include "desktop/tests/finite_playback_fixture.h"

namespace media_server
{

class finite_playback_view_test final : public QObject
{
    Q_OBJECT

   private slots:
    void initTestCase()
    {
        had_audio_driver_ = qEnvironmentVariableIsSet("SDL_AUDIODRIVER");
        old_audio_driver_ = qgetenv("SDL_AUDIODRIVER");
        QVERIFY(SDL_WasInit(0) == 0);
        QVERIFY(qputenv("SDL_AUDIODRIVER", "dummy"));
        QCOMPARE(SDL_InitSubSystem(SDL_INIT_AUDIO), 0);
        QCOMPARE(QByteArray(SDL_GetCurrentAudioDriver()), QByteArray("dummy"));
    }

    void cleanupTestCase()
    {
        SDL_Quit();
        if (had_audio_driver_)
        {
            qputenv("SDL_AUDIODRIVER", old_audio_driver_);
        }
        else
        {
            qunsetenv("SDL_AUDIODRIVER");
        }
    }

    void init() { QVERIFY(QCoreApplication::instance()->findChildren<decode_thread*>().isEmpty()); }

    void cleanup()
    {
        // 失败回收不能充当成功断言；各用例在返回前独立等待 decoder 析构。
        for (auto* decoder : QCoreApplication::instance()->findChildren<decode_thread*>())
        {
            decoder->requestInterruption();
            if (!decoder->wait(4000))
            {
                qFatal("有限回放测试失败回收时 decoder 未退出");
            }
            decoder->deleteLater();
        }
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QTRY_VERIFY_WITH_TIMEOUT(QCoreApplication::instance()->findChildren<decode_thread*>().isEmpty(), 3000);
    }

    void cancel_pending_data()
    {
        QTest::addColumn<bool>("destroy");
        QTest::newRow("stop") << false;
        QTest::newRow("destroy") << true;
    }

    void cancel_pending()
    {
        QFETCH(bool, destroy);
        http_fixture api, media;
        QVERIFY(api.start());
        QVERIFY(media.start());
        platform_client client;
        QVERIFY(client.set_base_url(api.url()));
        auto view = std::make_unique<playback_view>();
        view->play_record(client, historical_target(), 1760000000, 1760000002);
        const auto operations = operation_clients(client);
        QCOMPARE(operations.size(), qsizetype(1));
        const auto operation = operations.front();
        QTRY_COMPARE_WITH_TIMEOUT(api.requests.size(), qsizetype(1), 2000);
        QCOMPARE(api.requests[0].line, create_line);
        QCOMPARE(QJsonDocument::fromJson(api.requests[0].body).object(), (QJsonObject{{"start_time", 1760000000}, {"end_time", 1760000002}}));
        if (destroy)
        {
            view.reset();
        }
        else
        {
            view->stop();
        }
        QVERIFY(api.respond(0, 201, ticket(media)));
        QTRY_COMPARE_WITH_TIMEOUT(api.requests.size(), qsizetype(2), 2000);
        QCOMPARE(api.requests[1].line, delete_line());
        QVERIFY(api.respond(1, 204));
        QTRY_VERIFY_WITH_TIMEOUT(operation.isNull(), 2000);
        QCOMPARE(media.requests.size(), qsizetype(0));
        QVERIFY(QCoreApplication::instance()->findChildren<decode_thread*>().isEmpty());
        if (view)
        {
            QCOMPARE(view->status(), QStringLiteral("已停止"));
            QVERIFY(view->image().isNull());
        }
        QVERIFY(!api.parse_error && !media.parse_error);
        qInfo() << "迟到201: create=1 DELETE=1 mediaGET=0 decoder=0 destroy=" << destroy;
    }

    void late_response_keeps_original_platform()
    {
        http_fixture api_a, api_b, media;
        QVERIFY(api_a.start());
        QVERIFY(api_b.start());
        QVERIFY(media.start());
        platform_client client;
        QVERIFY(client.set_base_url(api_a.url()));
        auto view = std::make_unique<playback_view>();
        view->play_record(client, historical_target(), 1760000000, 1760000002);
        const auto operations = operation_clients(client);
        QCOMPARE(operations.size(), qsizetype(1));
        const auto operation = operations.front();
        QTRY_COMPARE_WITH_TIMEOUT(api_a.requests.size(), qsizetype(1), 2000);
        QCOMPARE(api_a.requests[0].line, create_line);
        QVERIFY(client.set_base_url(api_b.url()));
        view->stop();
        view = std::make_unique<playback_view>();
        QVERIFY(api_a.respond(0, 201, ticket(media)));
        QTRY_COMPARE_WITH_TIMEOUT(api_a.requests.size(), qsizetype(2), 2000);
        QCOMPARE(api_a.requests[1].line, delete_line());
        QVERIFY(api_a.respond(1, 204));
        QTRY_VERIFY_WITH_TIMEOUT(operation.isNull(), 2000);
        QCOMPARE(api_b.requests.size(), qsizetype(0));
        QCOMPARE(media.requests.size(), qsizetype(0));
        QVERIFY(QCoreApplication::instance()->findChildren<decode_thread*>().isEmpty());
        QVERIFY(!api_a.parse_error && !api_b.parse_error && !media.parse_error);
        qInfo() << "切平台+重建后A迟到201: A create/DELETE=1/1 B=0 mediaGET=0 decoder=0";
    }

    void active_stop_keeps_original_platform()
    {
        http_fixture api_a, api_b, media;
        QVERIFY(api_a.start());
        QVERIFY(api_b.start());
        QVERIFY(media.start());
        platform_client client;
        QVERIFY(client.set_base_url(api_a.url()));
        playback_view view;
        view.play_record(client, historical_target(), 1760000000, 1760000002);
        const auto operations = operation_clients(client);
        QCOMPARE(operations.size(), qsizetype(1));
        const auto operation = operations.front();
        QTRY_COMPARE_WITH_TIMEOUT(api_a.requests.size(), qsizetype(1), 2000);
        QVERIFY(api_a.respond(0, 201, ticket(media)));
        QTRY_COMPARE_WITH_TIMEOUT(media.requests.size(), qsizetype(1), 2000);
        QVERIFY(media.requests[0].line.startsWith("GET /play/gb28181/" + first_id.toUtf8() + "/"));
        const auto decoders = QCoreApplication::instance()->findChildren<decode_thread*>();
        QCOMPARE(decoders.size(), qsizetype(1));
        QPointer<decode_thread> decoder = decoders.front();
        QVERIFY(client.set_base_url(api_b.url()));
        QElapsedTimer elapsed;
        elapsed.start();
        view.stop();
        QVERIFY2(elapsed.elapsed() < 500, "stop 不应在 GUI 等待挂起的网络 decoder");
        QTRY_COMPARE_WITH_TIMEOUT(api_a.requests.size(), qsizetype(2), 2000);
        QCOMPARE(api_a.requests[1].line, delete_line());
        QVERIFY(api_a.respond(1, 204));
        QTRY_VERIFY_WITH_TIMEOUT(decoder.isNull(), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(media.requests[0].socket.isNull(), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(operation.isNull(), 2000);
        QCOMPARE(api_b.requests.size(), qsizetype(0));
        QCOMPARE(view.status(), QStringLiteral("已停止"));
        QVERIFY(!api_a.parse_error && !api_b.parse_error && !media.parse_error);
        qInfo() << "挂起GET切平台stop: A DELETE=1 B=0 decoder已析构";
    }

    void cleanup_pending_response()
    {
        http_fixture api, media;
        QVERIFY(api.start());
        QVERIFY(media.start());
        platform_client client;
        QVERIFY(client.set_base_url(api.url()));
        playback_view view;
        view.play_record(client, historical_target(), 1760000000, 1760000002);
        const auto operations = operation_clients(client);
        QCOMPARE(operations.size(), qsizetype(1));
        const auto operation = operations.front();
        QTRY_COMPARE_WITH_TIMEOUT(api.requests.size(), qsizetype(1), 2000);
        QVERIFY(api.respond(0, 502, {{"error", "playback_start_failed"}, {"playback_id", first_id}}));
        QTRY_COMPARE_WITH_TIMEOUT(api.requests.size(), qsizetype(2), 2000);
        QCOMPARE(api.requests[1].line, delete_line());
        QVERIFY(api.respond(1, 204));
        QTRY_VERIFY_WITH_TIMEOUT(operation.isNull(), 2000);
        QVERIFY(view.status().contains(QStringLiteral("录像回放申请失败")));
        QCOMPARE(media.requests.size(), qsizetype(0));
        QVERIFY(QCoreApplication::instance()->findChildren<decode_thread*>().isEmpty());
        QVERIFY(!api.parse_error && !media.parse_error);
        qInfo() << "502 cleanupPending补偿: DELETE=1 mediaGET=0 decoder=0";
    }

    void history_deadline_ticket_data()
    {
        QTest::addColumn<QString>("scenario");
        QTest::addColumn<bool>("valid");
        QTest::newRow("deadline-missing") << QStringLiteral("missing") << false;
        QTest::newRow("deadline-invalid") << QStringLiteral("invalid") << false;
        QTest::newRow("deadline-wrong-type") << QStringLiteral("type") << false;
        QTest::newRow("deadline-without-timezone") << QStringLiteral("local") << false;
        QTest::newRow("deadline-expired") << QStringLiteral("expired") << false;
        QTest::newRow("deadline-Go-RFC3339Nano-Z") << QStringLiteral("nano") << true;
        QTest::newRow("deadline-explicit-plus08") << QStringLiteral("offset") << true;
    }

    // 使用真实 request_history 入口：无效预算不创建 decoder，且仍回收平台独占实例。
    void history_deadline_ticket()
    {
        QFETCH(QString, scenario);
        QFETCH(bool, valid);
        http_fixture api, media;
        QVERIFY(api.start());
        QVERIFY(media.start());
        platform_client client;
        QVERIFY(client.set_base_url(api.url()));
        playback_view view;
        view.play_record(client, historical_target(), 1760000000, 1760000002);
        const auto operations = operation_clients(client);
        QCOMPARE(operations.size(), qsizetype(1));
        const auto operation = operations.front();
        QTRY_COMPARE_WITH_TIMEOUT(api.requests.size(), qsizetype(1), 2000);
        QCOMPARE(api.requests[0].line, create_line);
        auto response = ticket(media);
        const auto future = QDateTime::currentDateTimeUtc().addSecs(120);
        if (scenario == QStringLiteral("missing"))
        {
            response.remove(QStringLiteral("deadline"));
        }
        else if (scenario == QStringLiteral("invalid"))
        {
            response.insert(QStringLiteral("deadline"), QStringLiteral("not-a-timestamp"));
        }
        else if (scenario == QStringLiteral("type"))
        {
            response.insert(QStringLiteral("deadline"), 123);
        }
        else if (scenario == QStringLiteral("local"))
        {
            response.insert(QStringLiteral("deadline"), future.toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss")));
        }
        else if (scenario == QStringLiteral("expired"))
        {
            response.insert(QStringLiteral("deadline"), QDateTime::currentDateTimeUtc().addSecs(-1).toString(Qt::ISODateWithMs));
        }
        else if (scenario == QStringLiteral("nano"))
        {
            response.insert(QStringLiteral("deadline"), future.toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss")) + QStringLiteral(".123456789Z"));
        }
        else
        {
            QCOMPARE(scenario, QStringLiteral("offset"));
            response.insert(QStringLiteral("deadline"), future.toOffsetFromUtc(8 * 3600).toString(Qt::ISODateWithMs));
        }
        QVERIFY(api.respond(0, 201, response));
        if (valid)
        {
            QTRY_COMPARE_WITH_TIMEOUT(media.requests.size(), qsizetype(1), 2000);
            QCOMPARE(QCoreApplication::instance()->findChildren<decode_thread*>().size(), qsizetype(1));
            view.stop();
        }
        else
        {
            QTRY_VERIFY_WITH_TIMEOUT(view.status().contains(QStringLiteral("无效")), 2000);
        }
        QTRY_COMPARE_WITH_TIMEOUT(api.requests.size(), qsizetype(2), 2000);
        QCOMPARE(api.requests[1].line, delete_line());
        QVERIFY(api.respond(1, 204));
        QTRY_VERIFY_WITH_TIMEOUT(operation.isNull(), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(QCoreApplication::instance()->findChildren<decode_thread*>().isEmpty(), 3000);
        QCOMPARE(media.requests.size(), valid ? qsizetype(1) : qsizetype(0));
        QCOMPARE(api.requests.size(), qsizetype(2));
        QVERIFY(!api.parse_error && !media.parse_error);
    }

    void present_complete_ps_data()
    {
        QTest::addColumn<QString>("file_name");
        QTest::addColumn<bool>("audible");
        for (const auto& codec : {QStringLiteral("pcma"), QStringLiteral("aac")})
        {
            for (const auto& order : {QStringLiteral("video-first"), QStringLiteral("audio-first")})
            {
                for (const bool audible : {false, true})
                {
                    const auto name = codec + "-" + order + "-noend.ps";
                    QTest::newRow(qPrintable(name + (audible ? "-dummy" : "-muted"))) << name << audible;
                }
            }
        }
    }

    void present_complete_ps()
    {
        const auto folder = qEnvironmentVariable("FINITE_PS_TEST_FIXTURE_DIR");
        if (folder.isEmpty())
        {
            QSKIP("未指定私有PS素材；默认CTest不代表真实有限PS呈现验收");
        }
        QFETCH(QString, file_name);
        QFETCH(bool, audible);
        const auto path = QDir(folder).filePath(file_name);
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto payload = file.readAll();
        QVERIFY(!payload.isEmpty());

        QList<QByteArray> reference_hashes;
        qsizetype reference_pcm = 0;
        decode_reference(path, 50, reference_hashes, reference_pcm);
        if (QTest::currentTestFailed())
        {
            return;
        }
        const auto expected_samples = file_name.startsWith("aac") ? quint64(97280) : quint64(96000);
        QCOMPARE(reference_pcm, qsizetype(expected_samples * 4));

        http_fixture api, media;
        QVERIFY(api.start());
        QVERIFY(media.start());
        platform_client client;
        QVERIFY(client.set_base_url(api.url()));
        playback_view view;
        view.setFixedSize(320, 240);
        painted_frames painted(view);
        view.show();
        SDL_AudioDeviceID audio_id = 0;
        const auto close_audio = qScopeGuard(
            [&]()
            {
                view.stop();
                if (audio_id != 0)
                {
                    SDL_CloseAudioDevice(audio_id);
                }
            });
        view.play_record(client, historical_target(), 1760000000, 1760000002);
        const auto operations = operation_clients(client);
        QCOMPARE(operations.size(), qsizetype(1));
        const auto operation = operations.front();
        QTRY_COMPARE_WITH_TIMEOUT(api.requests.size(), qsizetype(1), 2000);
        QCOMPARE(api.requests[0].line, create_line);
        QVERIFY(api.respond(0, 201, ticket(media)));
        QTRY_COMPARE_WITH_TIMEOUT(media.requests.size(), qsizetype(1), 2000);
        QCOMPARE(media.requests[0].line, "GET /play/gb28181/" + first_id.toUtf8() + "/fixture-token.ps HTTP/1.1");
        QVERIFY(media.requests[0].body.isEmpty());
        const auto decoders = QCoreApplication::instance()->findChildren<decode_thread*>();
        QCOMPARE(decoders.size(), qsizetype(1));
        QPointer<decode_thread> decoder = decoders.front();
        if (audible)
        {
            SDL_AudioSpec wanted{};
            wanted.freq = 48000;
            wanted.format = AUDIO_S16SYS;
            wanted.channels = 2;
            wanted.samples = 512;
            SDL_AudioSpec obtained{};
            audio_id = SDL_OpenAudioDevice(nullptr, 0, &wanted, &obtained, 0);
            QVERIFY2(audio_id != 0, SDL_GetError());
            view.set_audible(audio_id, obtained.samples * 4);
        }
        QVERIFY(media.stream(0, payload));
        // 明确的呈现终态和 decoder 析构共同作为完成屏障，不以 snapshot.ended 冒充呈现完成。
        QTRY_VERIFY_WITH_TIMEOUT(view.status().startsWith(QStringLiteral("录像传输及呈现已结束")), 6000);
        QTRY_VERIFY_WITH_TIMEOUT(decoder.isNull(), 2000);
        QTRY_COMPARE_WITH_TIMEOUT(api.requests.size(), qsizetype(2), 2000);
        QCOMPARE(api.requests[1].line, delete_line());
        QVERIFY(api.respond(1, 204));
        QTRY_VERIFY_WITH_TIMEOUT(operation.isNull(), 2000);
        QTRY_COMPARE_WITH_TIMEOUT(painted.hashes.size(), reference_hashes.size(), 1000);
        QCOMPARE(painted.hashes, reference_hashes);
        for (qsizetype index = 0; index < painted.indices.size(); ++index)
        {
            QCOMPARE(painted.indices[index], quint64(index + 1));
        }
        const auto stats = view.snapshot();
        QVERIFY2(stats.error.isEmpty(), qPrintable(stats.error));
        QCOMPARE(stats.video_frames, quint64(50));
        QCOMPARE(view.rendered_frames(), quint64(50));
        QCOMPARE(stats.video_dropped, quint64(0));
        QCOMPARE(stats.audio_dropped, quint64(0));
        QCOMPARE(stats.pcm_samples, expected_samples);
        QVERIFY(!view.image().isNull());
        QVERIFY(!view.audible());
        if (audible)
        {
            QCOMPARE(SDL_GetQueuedAudioSize(audio_id), Uint32(0));
            QCOMPARE(SDL_GetAudioDeviceStatus(audio_id), SDL_AUDIO_PAUSED);
        }
        QCOMPARE(api.requests.size(), qsizetype(2));
        QCOMPARE(media.requests.size(), qsizetype(1));
        QVERIFY(!api.parse_error && !media.parse_error);
        qInfo().noquote() << file_name << "audible(dummy-only)=" << audible
                          << "painted/reference=50/50 create/GET/DELETE=1/1/1 decoder已析构 pid=" << QCoreApplication::applicationPid();
    }

    void pause_resume_ps_data()
    {
        QTest::addColumn<bool>("audible");
        QTest::addColumn<quint64>("pause_frame");
        // 前两行暂停时解码线程处于满队列背压；第三行已排空已发数据，阻塞在 HTTP 读取。
        QTest::newRow("muted-backpressure") << false << quint64(5);
        QTest::newRow("dummy-backpressure") << true << quint64(5);
        QTest::newRow("muted-network-wait") << false << quint64(125);
    }

    // 暂停超过 10 秒解码进展期限且无新数据；恢复后仍须完整、按序呈现全部帧，不丢帧。
    void pause_resume_ps()
    {
        const auto folder = qEnvironmentVariable("FINITE_PS_TEST_FIXTURE_DIR");
        if (folder.isEmpty())
        {
            QSKIP("未指定私有PS素材；默认CTest不代表真实有限PS暂停验收");
        }
        QFETCH(bool, audible);
        QFETCH(quint64, pause_frame);
        // 8 秒素材：连接保持打开时轨道探测无需等待 EOF。
        const auto path = QDir(folder).filePath(QStringLiteral("pcma-long-noend.ps"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto payload = file.readAll();
        QVERIFY(!payload.isEmpty());
        QList<QByteArray> reference_hashes;
        qsizetype reference_pcm = 0;
        decode_reference(path, 200, reference_hashes, reference_pcm);
        if (QTest::currentTestFailed())
        {
            return;
        }

        http_fixture api, media;
        QVERIFY(api.start());
        QVERIFY(media.start());
        platform_client client;
        QVERIFY(client.set_base_url(api.url()));
        playback_view view;
        view.setFixedSize(320, 240);
        painted_frames painted(view);
        view.show();
        SDL_AudioDeviceID audio_id = 0;
        const auto close_audio = qScopeGuard(
            [&]()
            {
                view.stop();
                if (audio_id != 0)
                {
                    SDL_CloseAudioDevice(audio_id);
                }
            });
        view.play_record(client, historical_target(), 1760000000, 1760000002);
        QTRY_COMPARE_WITH_TIMEOUT(api.requests.size(), qsizetype(1), 2000);
        QVERIFY(!view.record_control_ready());
        QVERIFY(api.respond(0, 201, ticket(media)));
        QTRY_COMPARE_WITH_TIMEOUT(media.requests.size(), qsizetype(1), 2000);
        const auto decoders = QCoreApplication::instance()->findChildren<decode_thread*>();
        QCOMPARE(decoders.size(), qsizetype(1));
        QPointer<decode_thread> decoder = decoders.front();
        if (audible)
        {
            SDL_AudioSpec wanted{};
            wanted.freq = 48000;
            wanted.format = AUDIO_S16SYS;
            wanted.channels = 2;
            wanted.samples = 512;
            SDL_AudioSpec obtained{};
            audio_id = SDL_OpenAudioDevice(nullptr, 0, &wanted, &obtained, 0);
            QVERIFY2(audio_id != 0, SDL_GetError());
            view.set_audible(audio_id, obtained.samples * 4);
        }
        const auto half = payload.size() * 7 / 10;
        QVERIFY(media.send(0, stream_header + chunk(payload.first(half))));
        QTRY_VERIFY_WITH_TIMEOUT(view.rendered_frames() >= pause_frame, 8000);
        QVERIFY(view.record_control_ready());

        view.set_record_paused(true);
        QTRY_COMPARE_WITH_TIMEOUT(api.requests.size(), qsizetype(2), 2000);
        QCOMPARE(api.requests[1].line, control_line("pause"));
        // 在途时不能再发控制。
        QVERIFY(!view.record_control_ready());
        view.set_record_paused(false);
        QVERIFY(api.respond(1, 204));
        QTRY_VERIFY_WITH_TIMEOUT(view.record_paused(), 2000);
        QVERIFY(view.record_control_ready());
        const auto frozen = view.rendered_frames();
        const auto frozen_queue = audible ? SDL_GetQueuedAudioSize(audio_id) : 0U;
        QTest::qWait(12000);
        QVERIFY2(decoder && !decoder->isFinished(), "暂停期间解码进展期限不能结束回放");
        QVERIFY2(view.snapshot().error.isEmpty(), qPrintable(view.snapshot().error));
        QCOMPARE(view.rendered_frames(), frozen);
        QVERIFY(view.status().contains(QStringLiteral("已暂停")));
        if (audible)
        {
            QCOMPARE(SDL_GetAudioDeviceStatus(audio_id), SDL_AUDIO_PAUSED);
            QVERIFY(SDL_GetQueuedAudioSize(audio_id) >= frozen_queue);
            QVERIFY(view.audible());
        }

        view.set_record_paused(false);
        QTRY_COMPARE_WITH_TIMEOUT(api.requests.size(), qsizetype(3), 2000);
        QCOMPARE(api.requests[2].line, control_line("resume"));
        QVERIFY(api.respond(2, 204));
        QTRY_VERIFY_WITH_TIMEOUT(!view.record_paused(), 2000);
        QVERIFY(media.send(0, chunk(payload.mid(half)) + "0\r\n\r\n"));
        media.requests[0].socket->disconnectFromHost();
        QTRY_VERIFY_WITH_TIMEOUT(view.status().startsWith(QStringLiteral("录像传输及呈现已结束")), 12000);
        QTRY_VERIFY_WITH_TIMEOUT(decoder.isNull(), 2000);
        QTRY_COMPARE_WITH_TIMEOUT(api.requests.size(), qsizetype(4), 2000);
        QCOMPARE(api.requests[3].line, delete_line());
        QVERIFY(api.respond(3, 204));
        QTRY_COMPARE_WITH_TIMEOUT(painted.hashes.size(), reference_hashes.size(), 1000);
        QCOMPARE(painted.hashes, reference_hashes);
        const auto stats = view.snapshot();
        QVERIFY2(stats.error.isEmpty(), qPrintable(stats.error));
        QCOMPARE(stats.video_dropped, quint64(0));
        QCOMPARE(stats.audio_dropped, quint64(0));
        QCOMPARE(stats.pcm_samples, quint64(8 * 48000));
        QCOMPARE(view.rendered_frames(), quint64(200));
        QVERIFY(!api.parse_error && !media.parse_error);
        qInfo().noquote() << "pause>10s audible(dummy-only)=" << audible << "frozen_frames=" << frozen
                          << "painted/reference=200/200 create/pause/resume/DELETE=1/1/1/1";
    }

    // 解码线程在尾部呈现前已结束，暂停与继续仍作用于剩余呈现。
    void resume_after_eof_while_paused()
    {
        const auto folder = qEnvironmentVariable("FINITE_PS_TEST_FIXTURE_DIR");
        if (folder.isEmpty())
        {
            QSKIP("未指定私有PS素材");
        }
        const auto path = QDir(folder).filePath(QStringLiteral("pcma-video-first-noend.ps"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto payload = file.readAll();
        QList<QByteArray> reference_hashes;
        qsizetype reference_pcm = 0;
        decode_reference(path, 50, reference_hashes, reference_pcm);
        if (QTest::currentTestFailed())
        {
            return;
        }
        http_fixture api, media;
        QVERIFY(api.start());
        QVERIFY(media.start());
        platform_client client;
        QVERIFY(client.set_base_url(api.url()));
        playback_view view;
        view.setFixedSize(320, 240);
        painted_frames painted(view);
        view.show();
        view.play_record(client, historical_target(), 1760000000, 1760000002);
        QTRY_COMPARE_WITH_TIMEOUT(api.requests.size(), qsizetype(1), 2000);
        auto response = ticket(media);
        const QDeadlineTimer transport_budget(2600, Qt::PreciseTimer);
        response.insert(QStringLiteral("deadline"), QDateTime::currentDateTimeUtc().addMSecs(2600).toString(Qt::ISODateWithMs));
        QVERIFY(api.respond(0, 201, response));
        QTRY_COMPARE_WITH_TIMEOUT(media.requests.size(), qsizetype(1), 2000);
        QPointer<decode_thread> decoder = QCoreApplication::instance()->findChildren<decode_thread*>().front();
        // 2 秒素材连同终止块一次发完；剩余帧落在有限队列内时解码线程先结束。
        QVERIFY(media.stream(0, payload));
        QTRY_VERIFY_WITH_TIMEOUT(view.rendered_frames() >= 35, 6000);
        view.set_record_paused(true);
        QTRY_COMPARE_WITH_TIMEOUT(api.requests.size(), qsizetype(2), 2000);
        QCOMPARE(api.requests[1].line, control_line("pause"));
        QVERIFY(api.respond(1, 204));
        QTRY_VERIFY_WITH_TIMEOUT(view.record_paused(), 2000);
        const auto frozen = view.rendered_frames();
        QTRY_VERIFY_WITH_TIMEOUT(decoder && decoder->isFinished(), 4000);
        QTRY_VERIFY_WITH_TIMEOUT(transport_budget.hasExpired(), 3000);
        QTest::qWait(100);
        QCOMPARE(view.rendered_frames(), frozen);
        QVERIFY(frozen < 50);
        QVERIFY(view.record_control_ready());
        view.set_record_paused(false);
        QTRY_COMPARE_WITH_TIMEOUT(api.requests.size(), qsizetype(3), 2000);
        QCOMPARE(api.requests[2].line, control_line("resume"));
        QVERIFY(api.respond(2, 204));
        QTRY_VERIFY_WITH_TIMEOUT(!view.record_paused(), 2000);
        QVERIFY(!view.record_seek_ready());
        view.scale_record(2);
        QCOMPARE(api.requests.size(), qsizetype(3));
        QTRY_VERIFY_WITH_TIMEOUT(view.status().startsWith(QStringLiteral("录像传输及呈现已结束")), 6000);
        QTRY_VERIFY_WITH_TIMEOUT(decoder.isNull(), 2000);
        QTRY_COMPARE_WITH_TIMEOUT(api.requests.size(), qsizetype(4), 2000);
        QCOMPARE(api.requests[3].line, delete_line());
        QVERIFY(api.respond(3, 204));
        QTRY_COMPARE_WITH_TIMEOUT(painted.hashes.size(), reference_hashes.size(), 1000);
        QCOMPARE(painted.hashes, reference_hashes);
        QCOMPARE(view.snapshot().video_dropped, quint64(0));
        QVERIFY(!api.parse_error && !media.parse_error);
        qInfo() << "暂停中解码结束: frozen=" << frozen << "继续后 painted/reference=50/50";
    }

    // 暂停未受理继续播放；恢复时实例已不存在则结束本次回放，不自动发相反指令。
    void control_failures()
    {
        const auto folder = qEnvironmentVariable("FINITE_PS_TEST_FIXTURE_DIR");
        if (folder.isEmpty())
        {
            QSKIP("未指定私有PS素材");
        }
        QFile file(QDir(folder).filePath(QStringLiteral("pcma-long-noend.ps")));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto payload = file.readAll();
        http_fixture api, media;
        QVERIFY(api.start());
        QVERIFY(media.start());
        platform_client client;
        QVERIFY(client.set_base_url(api.url()));
        playback_view view;
        QSignalSpy errors(&view, &playback_view::playback_error);
        view.play_record(client, historical_target(), 1760000000, 1760000002);
        QTRY_COMPARE_WITH_TIMEOUT(api.requests.size(), qsizetype(1), 2000);
        QVERIFY(api.respond(0, 201, ticket(media)));
        QTRY_COMPARE_WITH_TIMEOUT(media.requests.size(), qsizetype(1), 2000);
        QPointer<decode_thread> decoder = QCoreApplication::instance()->findChildren<decode_thread*>().front();
        QVERIFY(media.send(0, stream_header + chunk(payload.first(payload.size() * 7 / 10))));
        QTRY_VERIFY_WITH_TIMEOUT(view.rendered_frames() >= 2 && view.record_control_ready(), 4000);

        view.set_record_paused(true);
        QTRY_COMPARE_WITH_TIMEOUT(api.requests.size(), qsizetype(2), 2000);
        QVERIFY(api.respond(1, 502, {{"error", "device_rejected"}}));
        QTRY_COMPARE_WITH_TIMEOUT(errors.size(), qsizetype(1), 2000);
        QVERIFY(errors.back().front().toString().startsWith(QStringLiteral("暂停未确认")));
        QVERIFY(!view.record_paused());
        QVERIFY(view.record_control_ready());

        view.set_record_paused(true);
        QTRY_COMPARE_WITH_TIMEOUT(api.requests.size(), qsizetype(3), 2000);
        QVERIFY(api.respond(2, 204));
        QTRY_VERIFY_WITH_TIMEOUT(view.record_paused(), 2000);
        view.set_record_paused(false);
        QTRY_COMPARE_WITH_TIMEOUT(api.requests.size(), qsizetype(4), 2000);
        QVERIFY(api.respond(3, 409, {{"error", "playback_control_busy"}}));
        QTRY_COMPARE_WITH_TIMEOUT(errors.size(), qsizetype(2), 2000);
        QVERIFY(errors.back().front().toString().startsWith(QStringLiteral("继续未确认")));
        QVERIFY(view.record_paused());

        view.set_record_paused(false);
        QTRY_COMPARE_WITH_TIMEOUT(api.requests.size(), qsizetype(5), 2000);
        QCOMPARE(api.requests[4].line, control_line("resume"));
        QVERIFY(api.respond(4, 404, {{"error", "playback_not_found"}}));
        QTRY_COMPARE_WITH_TIMEOUT(view.status(), QStringLiteral("录像实例已结束或到期，已停止本次回放"), 2000);
        QTRY_COMPARE_WITH_TIMEOUT(api.requests.size(), qsizetype(6), 2000);
        QCOMPARE(api.requests[5].line, delete_line());
        QVERIFY(api.respond(5, 404, {{"error", "playback_not_found"}}));
        QTRY_VERIFY_WITH_TIMEOUT(decoder.isNull(), 3000);
        QVERIFY(!view.record_control_ready());
        QVERIFY(!api.parse_error && !media.parse_error);
        qInfo() << "控制失败: 502保持播放 409busy保持暂停 404结束并DELETE=1";
    }

    void seek_control_data()
    {
        QTest::addColumn<int>("status");
        QTest::newRow("accepted-but-media-unchanged") << 204;
        QTest::newRow("busy") << 409;
        QTest::newRow("rejected") << 502;
        QTest::newRow("expired") << 404;
        QTest::newRow("timeout") << 0;
    }

    void seek_control()
    {
        QFETCH(int, status);
        const auto folder = qEnvironmentVariable("FINITE_PS_TEST_FIXTURE_DIR");
        if (folder.isEmpty())
        {
            QSKIP("未指定私有PS素材");
        }
        const auto path = QDir(folder).filePath(QStringLiteral("pcma-long-noend.ps"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto payload = file.readAll();
        http_fixture api, media;
        QVERIFY(api.start());
        QVERIFY(media.start());
        platform_client client;
        QVERIFY(client.set_base_url(api.url()));
        playback_view view;
        view.resize(320, 240);
        view.show();
        painted_frames painted(view);
        QSignalSpy errors(&view, &playback_view::playback_error);
        QSignalSpy statuses(&view, &playback_view::status_changed);
        view.seek_record(1);
        QCOMPARE(api.requests.size(), qsizetype(0));
        view.play_record(client, historical_target(), 1760000000, 1760000008);
        QCOMPARE(view.record_start(), qint64(1760000000));
        QCOMPARE(view.record_end(), qint64(1760000008));
        QVERIFY(!view.record_seek_ready());
        view.seek_record(1);
        QTRY_COMPARE_WITH_TIMEOUT(api.requests.size(), qsizetype(1), 2000);
        QVERIFY(api.respond(0, 201, ticket(media)));
        QTRY_COMPARE_WITH_TIMEOUT(media.requests.size(), qsizetype(1), 2000);
        QPointer<decode_thread> decoder = QCoreApplication::instance()->findChildren<decode_thread*>().front();
        QVERIFY(media.send(0, stream_header + chunk(payload)));
        QTRY_VERIFY_WITH_TIMEOUT(view.rendered_frames() >= 3 && view.record_seek_ready(), 4000);
        for (const auto value : {-1.0, 8.001, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
        {
            view.seek_record(value);
        }
        QCOMPARE(api.requests.size(), qsizetype(1));
        view.seek_record(1.25);
        QVERIFY(!view.record_seek_ready());
        view.seek_record(2);
        view.set_record_paused(true);
        view.scale_record(2);
        QTRY_COMPARE_WITH_TIMEOUT(api.requests.size(), qsizetype(2), 2000);
        QCOMPARE(api.requests[1].line, control_line("seek"));
        QCOMPARE(QJsonDocument::fromJson(api.requests[1].body).object(), (QJsonObject{{"position", 1.25}}));
        if (status != 0)
        {
            QVERIFY(api.respond(1, status, status == 204 ? QJsonObject{} : QJsonObject{{"error", "device_rejected"}}));
        }
        if (status == 404)
        {
            QTRY_VERIFY_WITH_TIMEOUT(decoder.isNull(), 3000);
            QVERIFY(!view.record_control_ready());
        }
        else
        {
            QTRY_VERIFY_WITH_TIMEOUT(view.record_control_ready(), status == 0 ? 11000 : 2000);
            QVERIFY(!view.record_paused());
            QCOMPARE(QCoreApplication::instance()->findChildren<decode_thread*>().front(), decoder.data());
            if (status == 204)
            {
                QVERIFY(std::any_of(statuses.cbegin(),
                                    statuses.cend(),
                                    [](const auto& values) { return values.front().toString().contains(QStringLiteral("设备已受理定位请求")); }));
                for (const double value : {0.0, 8.0})
                {
                    const auto index = api.requests.size();
                    view.seek_record(value);
                    QTRY_COMPARE_WITH_TIMEOUT(api.requests.size(), index + 1, 2000);
                    QCOMPARE(QJsonDocument::fromJson(api.requests[index].body).object(), (QJsonObject{{"position", value}}));
                    QVERIFY(api.respond(index, 204));
                    QTRY_VERIFY_WITH_TIMEOUT(view.record_control_ready(), 2000);
                }
                QVERIFY(media.send(0, "0\r\n\r\n"));
                QTRY_VERIFY_WITH_TIMEOUT(view.snapshot().ended && !view.record_control_ready(), 11000);
                QTRY_VERIFY_WITH_TIMEOUT(decoder.isNull(), 2000);
                QCOMPARE(view.rendered_frames(), quint64(200));
                QList<QByteArray> expected;
                qsizetype pcm = 0;
                decode_reference(path, 200, expected, pcm);
                QVERIFY(!QTest::currentTestFailed());
                QCOMPARE(painted.hashes, expected);
                QCOMPARE(view.snapshot().pcm_samples, quint64(pcm / 4));
                QCOMPARE(view.snapshot().video_dropped, quint64(0));
                QCOMPARE(errors.size(), qsizetype(0));
            }
            else
            {
                QCOMPARE(errors.size(), qsizetype(1));
                QVERIFY(errors.front().front().toString().startsWith(QStringLiteral("定位未确认")));
                view.stop();
            }
        }
        const auto expected_requests = status == 204 ? qsizetype(5) : qsizetype(3);
        QTRY_COMPARE_WITH_TIMEOUT(api.requests.size(), expected_requests, 2000);
        QCOMPARE(api.requests.back().line, delete_line());
        QVERIFY(api.respond(api.requests.size() - 1, 204));
        QTRY_VERIFY_WITH_TIMEOUT(decoder.isNull(), 3000);
        QCOMPARE(media.requests.size(), qsizetype(1));
        QVERIFY(!api.parse_error && !media.parse_error);
        qInfo() << "定位控制 status=" << status << "create=1 GET=1 DELETE=1";
    }

    void scale_control_data()
    {
        QTest::addColumn<int>("status");
        QTest::addColumn<QString>("error_code");
        QTest::newRow("accepted-media-unchanged") << 204 << QString{};
        QTest::newRow("busy") << 409 << QStringLiteral("playback_control_busy");
        QTest::newRow("rejected") << 502 << QStringLiteral("device_rejected");
        QTest::newRow("expired") << 404 << QStringLiteral("playback_not_found");
        QTest::newRow("not-streaming") << 409 << QStringLiteral("playback_not_streaming");
        QTest::newRow("unexpected-200") << 200 << QString{};
        QTest::newRow("timeout") << 0 << QString{};
    }

    void scale_control()
    {
        QFETCH(int, status);
        QFETCH(QString, error_code);
        const auto folder = qEnvironmentVariable("FINITE_PS_TEST_FIXTURE_DIR");
        if (folder.isEmpty())
        {
            QSKIP("未指定私有PS素材");
        }
        const auto path = QDir(folder).filePath(QStringLiteral("pcma-long-noend.ps"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto payload = file.readAll();
        QList<QByteArray> expected;
        qsizetype reference_pcm = 0;
        if (status == 204)
        {
            decode_reference(path, 200, expected, reference_pcm);
            QVERIFY(!QTest::currentTestFailed());
            QCOMPARE(reference_pcm, qsizetype(384000 * 4));
        }
        http_fixture api, media;
        QVERIFY(api.start());
        QVERIFY(media.start());
        platform_client client;
        QVERIFY(client.set_base_url(api.url()));
        playback_view view;
        view.setFixedSize(320, 240);
        painted_frames painted(view);
        view.show();
        QSignalSpy errors(&view, &playback_view::playback_error);
        QSignalSpy statuses(&view, &playback_view::status_changed);
        view.scale_record(2);
        QCOMPARE(api.requests.size(), qsizetype(0));
        view.play_record(client, historical_target(), 1760000000, 1760000008);
        const auto operations = operation_clients(client);
        QCOMPARE(operations.size(), qsizetype(1));
        view.scale_record(2);
        QTRY_COMPARE_WITH_TIMEOUT(api.requests.size(), qsizetype(1), 2000);
        QVERIFY(api.respond(0, 201, ticket(media)));
        QTRY_COMPARE_WITH_TIMEOUT(media.requests.size(), qsizetype(1), 2000);
        QPointer<decode_thread> decoder = QCoreApplication::instance()->findChildren<decode_thread*>().front();
        QVERIFY(!view.record_seek_ready());
        view.scale_record(2);
        QTest::qWait(30);
        QCOMPARE(api.requests.size(), qsizetype(1));
        SDL_AudioSpec want{}, have{};
        want.freq = 48000;
        want.format = AUDIO_S16SYS;
        want.channels = 2;
        want.samples = 512;
        const auto audio_id = status == 204 ? SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0) : 0;
        const auto close_audio = qScopeGuard(
            [&]()
            {
                view.stop();
                if (audio_id != 0)
                {
                    SDL_CloseAudioDevice(audio_id);
                }
            });
        if (status == 204)
        {
            QVERIFY2(audio_id != 0, SDL_GetError());
            view.set_audible(audio_id, have.size);
        }
        QVERIFY(media.send(0, stream_header + chunk(payload)));
        QTRY_VERIFY_WITH_TIMEOUT(view.rendered_frames() >= 3 && view.record_seek_ready(), 4000);
        for (const auto value : {-1.0,
                                 0.0,
                                 0.249,
                                 0.75,
                                 1.5,
                                 4.001,
                                 std::numeric_limits<double>::infinity(),
                                 -std::numeric_limits<double>::infinity(),
                                 std::numeric_limits<double>::quiet_NaN()})
        {
            view.scale_record(value);
        }
        QCOMPARE(api.requests.size(), qsizetype(1));
        view.set_record_paused(true);
        view.scale_record(2);
        QTRY_COMPARE_WITH_TIMEOUT(api.requests.size(), qsizetype(2), 2000);
        QCOMPARE(api.requests[1].line, control_line("pause"));
        QVERIFY(api.respond(1, 204));
        QTRY_VERIFY_WITH_TIMEOUT(view.record_paused(), 2000);
        view.scale_record(2);
        QTest::qWait(50);
        QCOMPARE(api.requests.size(), qsizetype(2));
        view.set_record_paused(false);
        view.scale_record(2);
        QTRY_COMPARE_WITH_TIMEOUT(api.requests.size(), qsizetype(3), 2000);
        QCOMPARE(api.requests[2].line, control_line("resume"));
        QVERIFY(api.respond(2, 204));
        QTRY_VERIFY_WITH_TIMEOUT(view.record_seek_ready(), 2000);
        QElapsedTimer elapsed;
        elapsed.start();
        view.scale_record(2);
        QVERIFY(!view.record_seek_ready());
        view.scale_record(4);
        view.seek_record(1);
        view.set_record_paused(true);
        QTRY_COMPARE_WITH_TIMEOUT(api.requests.size(), qsizetype(4), 2000);
        QCOMPARE(api.requests[3].line, control_line("scale"));
        QCOMPARE(QJsonDocument::fromJson(api.requests[3].body).object(), (QJsonObject{{"scale", 2}}));
        if (status != 0)
        {
            QVERIFY(api.respond(3, status, error_code.isEmpty() ? QJsonObject{} : QJsonObject{{"error", error_code}}));
        }
        const bool ended = status == 404 || error_code == QStringLiteral("playback_not_streaming");
        if (ended)
        {
            QTRY_COMPARE_WITH_TIMEOUT(view.status(), QStringLiteral("录像实例已结束或到期，已停止本次回放"), 2000);
            QVERIFY(!view.record_control_ready());
        }
        else if (status == 204)
        {
            QTRY_VERIFY_WITH_TIMEOUT(view.record_seek_ready(), 2000);
            QVERIFY(!view.record_paused());
            QCOMPARE(QCoreApplication::instance()->findChildren<decode_thread*>().front(), decoder.data());
            QVERIFY(view.audible());
            QVERIFY(std::any_of(statuses.cbegin(),
                                statuses.cend(),
                                [](const auto& values) { return values.front().toString().contains(QStringLiteral("设备已受理倍速请求")); }));
            for (const double value : {0.25, 0.5, 1.0, 4.0})
            {
                const auto index = api.requests.size();
                view.scale_record(value);
                QTRY_COMPARE_WITH_TIMEOUT(api.requests.size(), index + 1, 2000);
                QCOMPARE(api.requests[index].line, control_line("scale"));
                QCOMPARE(QJsonDocument::fromJson(api.requests[index].body).object(), (QJsonObject{{"scale", value}}));
                QVERIFY(api.respond(index, 204));
                QTRY_VERIFY_WITH_TIMEOUT(view.record_seek_ready(), 2000);
                QCOMPARE(QCoreApplication::instance()->findChildren<decode_thread*>().front(), decoder.data());
            }
            QVERIFY(media.send(0, "0\r\n\r\n"));
            QTRY_VERIFY_WITH_TIMEOUT(view.status().startsWith(QStringLiteral("录像传输及呈现已结束")), 11000);
            QVERIFY2(view.snapshot().error.isEmpty(), qPrintable(view.snapshot().error));
            QCOMPARE(view.snapshot().video_frames, quint64(200));
            QCOMPARE(view.rendered_frames(), quint64(200));
            QCOMPARE(painted.hashes, expected);
            QCOMPARE(view.snapshot().audio_samples, quint64(64000));
            QCOMPARE(view.snapshot().pcm_samples, quint64(reference_pcm / 4));
            QCOMPARE(view.snapshot().video_dropped, quint64(0));
            QCOMPARE(view.snapshot().audio_dropped, quint64(0));
            QCOMPARE(SDL_GetQueuedAudioSize(audio_id), Uint32(0));
            QCOMPARE(SDL_GetAudioDeviceStatus(audio_id), SDL_AUDIO_PAUSED);
            QVERIFY(!view.audible());
            // burst不携带真实供给速率；原1x耗时断言改由定时1x对照验证，保留全部帧/PCM完整性。
            qInfo() << "输入未变的burst完整排空 elapsed_ms=" << elapsed.elapsed() << "；非定时1x或请求倍率验收";
            QCOMPARE(errors.size(), qsizetype(0));
        }
        else
        {
            QTRY_COMPARE_WITH_TIMEOUT(errors.size(), qsizetype(1), status == 0 ? 11000 : 2000);
            QVERIFY(errors.front().front().toString().startsWith(QStringLiteral("倍速结果未知")));
            QVERIFY(!view.record_paused());
            QTest::qWait(100);
            QCOMPARE(api.requests.size(), qsizetype(4));
            QCOMPARE(QCoreApplication::instance()->findChildren<decode_thread*>().front(), decoder.data());
            QVERIFY(view.record_control_ready());
            view.stop();
        }
        const auto requests = status == 204 ? qsizetype(9) : qsizetype(5);
        QTRY_COMPARE_WITH_TIMEOUT(api.requests.size(), requests, 2000);
        QCOMPARE(api.requests.back().line, delete_line());
        QVERIFY(api.respond(requests - 1, 204));
        QTRY_VERIFY_WITH_TIMEOUT(decoder.isNull() && operations.front().isNull(), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(media.requests[0].socket.isNull(), 2000);
        view.scale_record(2);
        QTest::qWait(50);
        QCOMPARE(api.requests.size(), requests);
        QCOMPARE(media.requests.size(), qsizetype(1));
        QVERIFY(!api.parse_error && !media.parse_error);
        qInfo() << "倍速控制 status=" << status << "error=" << error_code << "create/GET/DELETE=1/1/1; 不代表倍速呈现";
    }

    void seek_stop_replacement_keeps_owner_data()
    {
        QTest::addColumn<bool>("scale");
        QTest::addColumn<bool>("replace");
        QTest::newRow("seek-replace") << false << true;
        QTest::newRow("scale-stop") << true << false;
        QTest::newRow("scale-replace") << true << true;
    }

    void seek_stop_replacement_keeps_owner()
    {
        QFETCH(bool, scale);
        QFETCH(bool, replace);
        const auto folder = qEnvironmentVariable("FINITE_PS_TEST_FIXTURE_DIR");
        if (folder.isEmpty())
        {
            QSKIP("未指定私有PS素材");
        }
        QFile file(QDir(folder).filePath(QStringLiteral("pcma-long-noend.ps")));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto payload = file.readAll();
        http_fixture api_a, api_b, media;
        QVERIFY(api_a.start());
        QVERIFY(api_b.start());
        QVERIFY(media.start());
        platform_client client;
        QVERIFY(client.set_base_url(api_a.url()));
        playback_view view;
        view.play_record(client, historical_target(), 1760000000, 1760000008);
        const auto operations = operation_clients(client);
        QCOMPARE(operations.size(), qsizetype(1));
        QTRY_COMPARE_WITH_TIMEOUT(api_a.requests.size(), qsizetype(1), 2000);
        QVERIFY(api_a.respond(0, 201, ticket(media)));
        QTRY_COMPARE_WITH_TIMEOUT(media.requests.size(), qsizetype(1), 2000);
        QVERIFY(media.send(0, stream_header + chunk(payload)));
        QTRY_VERIFY_WITH_TIMEOUT(view.record_seek_ready(), 4000);
        QPointer<decode_thread> old_decoder = QCoreApplication::instance()->findChildren<decode_thread*>().front();
        QVERIFY(client.set_base_url(api_b.url()));
        if (scale)
        {
            view.scale_record(2);
        }
        else
        {
            view.seek_record(3);
        }
        QTRY_COMPARE_WITH_TIMEOUT(api_a.requests.size(), qsizetype(2), 2000);
        QCOMPARE(api_a.requests[1].line, control_line(scale ? "scale" : "seek"));
        QCOMPARE(QJsonDocument::fromJson(api_a.requests[1].body).object(), scale ? (QJsonObject{{"scale", 2}}) : (QJsonObject{{"position", 3}}));
        QCOMPARE(api_b.requests.size(), qsizetype(0));
        if (replace)
        {
            view.play_record(client, historical_target(), 1760000010, 1760000012);
        }
        else
        {
            view.stop();
        }
        const auto status = view.status();
        // 响应与 stop 竞态：不要求已取消的客户端真实收到该 204。
        if (api_a.requests[1].socket)
        {
            api_a.respond(1, 204);
        }
        QTRY_COMPARE_WITH_TIMEOUT(api_a.requests.size(), qsizetype(3), 2000);
        QCOMPARE(api_a.requests[2].line, delete_line());
        QVERIFY(api_a.respond(2, 204));
        if (!replace)
        {
            QTRY_VERIFY_WITH_TIMEOUT(old_decoder.isNull() && operations.front().isNull(), 3000);
            QTest::qWait(100);
            QCOMPARE(view.status(), status);
            QCOMPARE(status, QStringLiteral("已停止"));
            QCOMPARE(api_b.requests.size(), qsizetype(0));
            QCOMPARE(api_a.requests.size(), qsizetype(3));
            QCOMPARE(media.requests.size(), qsizetype(1));
            QTRY_VERIFY_WITH_TIMEOUT(media.requests[0].socket.isNull(), 2000);
            QVERIFY(!api_a.parse_error && !api_b.parse_error && !media.parse_error);
            return;
        }
        QTRY_COMPARE_WITH_TIMEOUT(api_b.requests.size(), qsizetype(1), 2000);
        QCOMPARE(view.status(), status);
        QVERIFY(!view.record_paused());
        QVERIFY(!view.record_seek_ready());
        QCOMPARE(view.record_start(), qint64(1760000010));
        QTRY_VERIFY_WITH_TIMEOUT(old_decoder.isNull() && operations.front().isNull(), 3000);
        const auto new_operations = operation_clients(client);
        QCOMPARE(new_operations.size(), qsizetype(1));
        const auto new_owner = new_operations.front();
        const auto second_id = QStringLiteral("22222222-2222-4222-8222-222222222222");
        QVERIFY(api_b.respond(0, 201, ticket(media, second_id)));
        QTRY_COMPARE_WITH_TIMEOUT(media.requests.size(), qsizetype(2), 2000);
        const auto new_decoders = QCoreApplication::instance()->findChildren<decode_thread*>();
        QCOMPARE(new_decoders.size(), qsizetype(1));
        QPointer<decode_thread> new_decoder = new_decoders.front();
        file.close();
        file.setFileName(QDir(folder).filePath(QStringLiteral("pcma-video-first-noend.ps")));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY(media.stream(1, file.readAll()));
        QTRY_VERIFY_WITH_TIMEOUT(view.rendered_frames() == 50 && !view.record_control_ready(), 6000);
        QTRY_COMPARE_WITH_TIMEOUT(api_b.requests.size(), qsizetype(2), 2000);
        QCOMPARE(api_b.requests[1].line, delete_line(second_id));
        QVERIFY(api_b.respond(1, 204));
        QTRY_VERIFY_WITH_TIMEOUT(new_owner.isNull() && new_decoder.isNull(), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(media.requests[0].socket.isNull() && media.requests[1].socket.isNull(), 2000);
        QCOMPARE(api_a.requests.size(), qsizetype(3));
        QCOMPARE(media.requests.size(), qsizetype(2));
        QVERIFY(!api_a.parse_error && !api_b.parse_error && !media.parse_error);
        qInfo() << "定位pending平台切换：控制/DELETE固定A，late响应不改变B，B独立GET/DELETE";
    }

    void timestamp_jumps_data()
    {
        QTest::addColumn<QString>("order");
        QTest::addColumn<bool>("audible");
        QTest::addColumn<bool>("scaled");
        QTest::newRow("video-first-muted") << QStringLiteral("video") << false << false;
        QTest::newRow("audio-first-muted") << QStringLiteral("audio") << false << false;
        QTest::newRow("video-first-audible") << QStringLiteral("video") << true << false;
        QTest::newRow("audio-first-audible") << QStringLiteral("audio") << true << false;
        QTest::newRow("scale-video-first-muted") << QStringLiteral("video") << false << true;
        QTest::newRow("scale-audio-first-audible") << QStringLiteral("audio") << true << true;
    }

    void timestamp_jumps()
    {
        QFETCH(QString, order);
        QFETCH(bool, audible);
        QFETCH(bool, scaled);
        const auto folder = qEnvironmentVariable("FINITE_PS_TEST_FIXTURE_DIR");
        if (folder.isEmpty())
        {
            QSKIP("未指定私有PS素材");
        }
        const auto path = QDir(folder).filePath(QStringLiteral("pcma-%1-first-noend.ps").arg(order));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto payload = file.readAll();
        QList<QByteArray> expected;
        qsizetype reference_pcm = 0;
        decode_reference(path, 50, expected, reference_pcm);
        QVERIFY(!QTest::currentTestFailed());
        http_fixture api, media;
        QVERIFY(api.start());
        QVERIFY(media.start());
        platform_client client;
        QVERIFY(client.set_base_url(api.url()));
        playback_view view;
        view.resize(320, 240);
        view.show();
        painted_frames painted(view);
        QSignalSpy errors(&view, &playback_view::playback_error);
        bool lost_audio_while_active = false;
        connect(&view,
                &playback_view::audible_changed,
                &view,
                [&](bool active)
                {
                    if (!active && view.record_control_ready())
                    {
                        lost_audio_while_active = true;
                    }
                });
        SDL_AudioSpec want{}, have{};
        want.freq = 48000;
        want.format = AUDIO_S16SYS;
        want.channels = 2;
        want.samples = 512;
        const auto audio_id = audible ? SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0) : 0;
        const auto close_audio = qScopeGuard(
            [&]()
            {
                view.stop();
                if (audio_id != 0)
                {
                    SDL_CloseAudioDevice(audio_id);
                }
            });
        QVERIFY(!audible || audio_id != 0);
        view.play_record(client, historical_target(), 1760000000, 1760000040);
        if (audible)
        {
            view.set_audible(audio_id, have.size);
        }
        QTRY_COMPARE_WITH_TIMEOUT(api.requests.size(), qsizetype(1), 2000);
        QVERIFY(api.respond(0, 201, ticket(media)));
        QTRY_COMPARE_WITH_TIMEOUT(media.requests.size(), qsizetype(1), 2000);
        QPointer<decode_thread> decoder = QCoreApplication::instance()->findChildren<decode_thread*>().front();
        QElapsedTimer elapsed;
        elapsed.start();
        // 一个 GET 内先向前跳20秒，再回到原始PTS；既不是新连接，也不等待控制响应来定义边界。
        QVERIFY(media.stream(0, payload + shifted_pes_timestamps(payload, 20 * 90000) + payload));
        if (scaled)
        {
            QTRY_VERIFY_WITH_TIMEOUT(view.rendered_frames() >= 3 && view.record_seek_ready(), 2000);
            view.scale_record(1);
            QTRY_COMPARE_WITH_TIMEOUT(api.requests.size(), qsizetype(2), 2000);
            QCOMPARE(api.requests[1].line, control_line("scale"));
            QVERIFY(api.respond(1, 204));
        }
        QTRY_VERIFY_WITH_TIMEOUT(view.snapshot().ended, 11000);
        QTRY_VERIFY_WITH_TIMEOUT(decoder.isNull(), 2000);
        QVERIFY2(view.snapshot().error.isEmpty(), qPrintable(view.snapshot().error));
        QCOMPARE(view.snapshot().video_frames, quint64(150));
        QCOMPARE(view.rendered_frames(), quint64(150));
        QCOMPARE(painted.hashes, expected + expected + expected);
        QCOMPARE(view.snapshot().video_dropped, quint64(0));
        // 两次真实跳变会丢弃 SWR 的旧滤波尾部，而不是拼接到下一段；正常 EOF 仍 drain。
        QVERIFY(view.snapshot().pcm_samples <= quint64(reference_pcm * 3 / 4));
        QVERIFY(view.snapshot().pcm_samples >= quint64(reference_pcm * 3 / 4 - 200));
        QCOMPARE(view.snapshot().audio_dropped, quint64(2));
        QCOMPARE(errors.size(), qsizetype(0));
        QVERIFY(!lost_audio_while_active);
        QVERIFY(elapsed.elapsed() < 10000);
        if (!scaled)
        {
            QVERIFY(elapsed.elapsed() >= 5000);
        }
        QCOMPARE(media.requests.size(), qsizetype(1));
        const auto deleted = scaled ? qsizetype(2) : qsizetype(1);
        QTRY_COMPARE_WITH_TIMEOUT(api.requests.size(), deleted + 1, 2000);
        QCOMPARE(api.requests[deleted].line, delete_line());
        QVERIFY(api.respond(deleted, 204));
        qInfo() << "同GET前后跳变 frames=150 PCM=" << view.snapshot().pcm_samples << "elapsed_ms=" << elapsed.elapsed() << "audible=" << audible;
    }

    void timestamp_tracks_diverge_data()
    {
        QTest::addColumn<bool>("audible");
        QTest::addColumn<bool>("video_back");
        QTest::addColumn<bool>("scaled");
        QTest::newRow("video-back-muted") << false << true << false;
        QTest::newRow("video-back-audible") << true << true << false;
        QTest::newRow("audio-back-muted") << false << false << false;
        QTest::newRow("audio-back-audible") << true << false << false;
        QTest::newRow("scale-video-back-muted") << false << true << true;
        QTest::newRow("scale-video-back-switch-mute") << true << true << true;
    }

    void timestamp_tracks_diverge()
    {
        QFETCH(bool, audible);
        QFETCH(bool, video_back);
        QFETCH(bool, scaled);
        const auto folder = qEnvironmentVariable("FINITE_PS_TEST_FIXTURE_DIR");
        if (folder.isEmpty())
        {
            QSKIP("未指定私有PS素材");
        }
        const auto path = QDir(folder).filePath(QStringLiteral("pcma-video-first-noend.ps"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto payload = file.readAll();
        QList<QByteArray> expected;
        qsizetype reference_pcm = 0;
        decode_reference(path, 50, expected, reference_pcm);
        QVERIFY(!QTest::currentTestFailed());
        http_fixture api, media;
        QVERIFY(api.start());
        QVERIFY(media.start());
        platform_client client;
        QVERIFY(client.set_base_url(api.url()));
        playback_view view;
        view.resize(320, 240);
        view.show();
        painted_frames painted(view);
        QSignalSpy errors(&view, &playback_view::playback_error);
        bool lost_audio_while_active = false;
        bool intentional_mute = false;
        connect(&view,
                &playback_view::audible_changed,
                &view,
                [&](bool active)
                {
                    if (!active && view.record_control_ready() && !intentional_mute)
                    {
                        lost_audio_while_active = true;
                    }
                });
        SDL_AudioSpec want{}, have{};
        want.freq = 48000;
        want.format = AUDIO_S16SYS;
        want.channels = 2;
        want.samples = 512;
        const auto audio_id = audible ? SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0) : 0;
        const auto close_audio = qScopeGuard(
            [&]()
            {
                view.stop();
                if (audio_id != 0)
                {
                    SDL_CloseAudioDevice(audio_id);
                }
            });
        QVERIFY(!audible || audio_id != 0);
        view.play_record(client, historical_target(), 1760000000, 1760000040);
        if (audible)
        {
            view.set_audible(audio_id, have.size);
        }
        QTRY_COMPARE_WITH_TIMEOUT(api.requests.size(), qsizetype(1), 2000);
        QVERIFY(api.respond(0, 201, ticket(media)));
        QTRY_COMPARE_WITH_TIMEOUT(media.requests.size(), qsizetype(1), 2000);
        QPointer<decode_thread> decoder = QCoreApplication::instance()->findChildren<decode_thread*>().front();
        QElapsedTimer elapsed;
        elapsed.start();
        // 首段双轨约20..22秒；第二段仅一轨回跳至0..2秒，另一轨连续22..24秒。
        // 同一个 GET 的真实负载不变；不能用视频 epoch 等待已经排空的音频 PTS。
        QVERIFY(media.stream(0, shifted_pes_timestamps(payload, 20 * 90000) + shifted_pes_timestamps(payload, 22 * 90000, !video_back, video_back)));
        if (scaled)
        {
            QTRY_VERIFY_WITH_TIMEOUT(view.rendered_frames() >= 3 && view.record_seek_ready(), 2000);
            view.scale_record(1);
            QTRY_COMPARE_WITH_TIMEOUT(api.requests.size(), qsizetype(2), 2000);
            QCOMPARE(api.requests[1].line, control_line("scale"));
            QVERIFY(api.respond(1, 204));
            if (audible)
            {
                QTRY_VERIFY_WITH_TIMEOUT(view.rendered_frames() >= 60, 4000);
                intentional_mute = true;
                view.set_audible(0, 0);
                QVERIFY(!view.audible());
            }
        }
        QTRY_COMPARE_WITH_TIMEOUT(view.rendered_frames(), quint64(100), 7000);
        QTRY_VERIFY_WITH_TIMEOUT(view.snapshot().ended, 2000);
        QVERIFY2(view.snapshot().error.isEmpty(), qPrintable(view.snapshot().error));
        QCOMPARE(view.snapshot().video_frames, quint64(100));
        QTRY_COMPARE_WITH_TIMEOUT(painted.hashes.size(), expected.size() * 2, 1000);
        QCOMPARE(painted.hashes, expected + expected);
        QCOMPARE(view.snapshot().video_dropped, quint64(0));
        // 本素材的8k→48k SWR滤波尾为96个输出样本；仅真实音频回跳会丢弃这段旧delay。
        QCOMPARE(view.snapshot().pcm_samples, quint64(reference_pcm * 2 / 4 - (video_back ? 0 : 96)));
        QCOMPARE(view.snapshot().audio_dropped, quint64(video_back ? 0 : 1));
        QCOMPARE(errors.size(), qsizetype(0));
        QVERIFY(!lost_audio_while_active);
        QTRY_VERIFY_WITH_TIMEOUT(decoder.isNull(), 2000);
        QVERIFY(elapsed.elapsed() < 7000);
        if (!scaled)
        {
            QVERIFY(elapsed.elapsed() >= 3000);
        }
        QCOMPARE(media.requests.size(), qsizetype(1));
        const auto deleted = scaled ? qsizetype(2) : qsizetype(1);
        QTRY_COMPARE_WITH_TIMEOUT(api.requests.size(), deleted + 1, 2000);
        QCOMPARE(api.requests[deleted].line, delete_line());
        QVERIFY(api.respond(deleted, 204));
        qInfo() << "同GET单轨回跳 frames=100 PCM=" << view.snapshot().pcm_samples << "elapsed_ms=" << elapsed.elapsed() << "audible=" << audible
                << "video_back=" << video_back;
    }

   private:
    // 同 SDK 本地有限解码作为呈现对照；不是独立编解码正确性证明。
    void decode_reference(const QString& path, qsizetype frames, QList<QByteArray>& hashes, qsizetype& pcm)
    {
        decode_thread reference(QUrl::fromLocalFile(path), QSize(320, 240), playback_mode::finite);
        const auto stop_reference = qScopeGuard(
            [&]()
            {
                reference.requestInterruption();
                if (!reference.wait(4000))
                {
                    qFatal("呈现参考解码线程没有退出");
                }
            });
        const auto consume_reference = [&]()
        {
            for (const auto& frame : reference.take_video_frames())
            {
                const auto& image = frame.image;
                hashes.append(QCryptographicHash::hash(QByteArray(reinterpret_cast<const char*>(image.constBits()), image.sizeInBytes()),
                                                       QCryptographicHash::Sha256));
            }
            for (const auto& block : reference.take_audio_blocks())
            {
                pcm += block.pcm.size();
            }
        };
        QTimer reference_consumer;
        connect(&reference_consumer, &QTimer::timeout, this, consume_reference);
        reference_consumer.start(1);
        reference.start();
        QTRY_VERIFY_WITH_TIMEOUT(reference.isFinished(), 4000);
        reference_consumer.stop();
        consume_reference();
        QVERIFY2(reference.snapshot().error.isEmpty(), qPrintable(reference.snapshot().error));
        QCOMPARE(hashes.size(), frames);
    }

   private:
    bool had_audio_driver_{};
    QByteArray old_audio_driver_;
};

}    // namespace media_server

QTEST_MAIN(media_server::finite_playback_view_test)
#include "finite_playback_view_test.moc"
