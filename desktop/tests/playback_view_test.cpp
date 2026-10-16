#include <memory>

#include <QUrl>
#include <SDL.h>
#include <QTest>
#include <QString>
#include <QPointer>
#include <QByteArray>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QDir>
#include <QTemporaryDir>
#include <QJsonArray>
#include <QScopeGuard>
#include <QJsonObject>
#include <QHostAddress>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QElapsedTimer>
#include <QAbstractSocket>
#include <QCoreApplication>

#include "desktop/decode_thread.h"
#include "desktop/record_thread.h"
#include "desktop/playback_view.h"
#include "desktop/platform_client.h"

namespace media_server
{

namespace
{

bool complete_request(QTcpSocket* peer)
{
    const auto bytes = peer->peek(65536);
    const auto end = bytes.indexOf("\r\n\r\n");
    if (end < 0)
    {
        return false;
    }
    for (const auto& line : bytes.first(end).split('\n'))
    {
        if (line.toLower().startsWith("content-length:"))
        {
            bool valid = false;
            const auto length = line.mid(sizeof("content-length:") - 1).trimmed().toLongLong(&valid);
            return valid && length >= 0 && bytes.size() >= end + 4 + length;
        }
    }
    return bytes.startsWith("GET ");
}

qint64 respond(QTcpSocket* peer, int status, const QJsonObject& document)
{
    const auto body = QJsonDocument(document).toJson(QJsonDocument::Compact);
    return peer->write("HTTP/1.1 " + QByteArray::number(status) + " Test\r\nContent-Type: application/json\r\nContent-Length: " +
                       QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
}

play_target target(const QString& name) { return {play_target::kind::source, name, {}, name}; }

QJsonObject channels(const QString& live_id, bool ready, const QString& state = QStringLiteral("streaming"))
{
    const QJsonObject other_live{{"live_id", "other-live"}, {"state", "streaming"}, {"media_ready", true}};
    const QJsonObject live{{"live_id", live_id}, {"state", state}, {"media_ready", ready}};
    const QJsonArray values{QJsonObject{{"channel_id", "other-channel"}, {"live", other_live}},
                            QJsonObject{{"channel_id", "channel"}, {"live", live}}};
    return {{"channels", values}};
}

}    // namespace

class playback_view_test final : public QObject
{
    Q_OBJECT

   private slots:
    void borrowed_audio_handoff_data()
    {
        QTest::addColumn<QString>("cleanup");
        QTest::addColumn<bool>("playing");
        for (const auto& action : {QStringLiteral("stop"), QStringLiteral("repeated-stop"), QStringLiteral("destroy")})
        {
            QTest::newRow(qPrintable(action + QStringLiteral("-paused"))) << action << false;
            QTest::newRow(qPrintable(action + QStringLiteral("-playing"))) << action << true;
        }
    }

    void borrowed_audio_handoff()
    {
        QFETCH(QString, cleanup);
        QFETCH(bool, playing);
        QVERIFY(SDL_WasInit(0) == 0);
        const bool had_driver = qEnvironmentVariableIsSet("SDL_AUDIODRIVER");
        const auto previous_driver = qgetenv("SDL_AUDIODRIVER");
        const auto restore_driver = qScopeGuard(
            [&]()
            {
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
        const auto quit_audio = qScopeGuard([]() { SDL_Quit(); });
        QCOMPARE(QByteArray(SDL_GetCurrentAudioDriver()), QByteArray("dummy"));
        SDL_AudioSpec wanted{};
        wanted.freq = 48'000;
        wanted.format = AUDIO_S16SYS;
        wanted.channels = 2;
        wanted.samples = 512;
        SDL_AudioSpec obtained{};
        const auto id = SDL_OpenAudioDevice(nullptr, 0, &wanted, &obtained, 0);
        QVERIFY2(id != 0, SDL_GetError());
        const auto close_audio = qScopeGuard([&]() { SDL_CloseAudioDevice(id); });
        auto former = std::make_unique<playback_view>();
        playback_view current;
        QSignalSpy former_audio(former.get(), &playback_view::audible_changed);
        QSignalSpy current_audio(&current, &playback_view::audible_changed);
        const QByteArray pcm(19'200, '\x24');
        former->set_audible(id, obtained.samples * 4);
        QVERIFY(former->audible());
        QCOMPARE(SDL_GetAudioDeviceStatus(id), SDL_AUDIO_PAUSED);
        QCOMPARE(SDL_QueueAudio(id, pcm.constData(), static_cast<Uint32>(pcm.size())), 0);
        QCOMPARE(SDL_GetQueuedAudioSize(id), static_cast<Uint32>(pcm.size()));

        former->set_audible(0, 0);
        QVERIFY(!former->audible());
        QCOMPARE(SDL_GetAudioDeviceStatus(id), SDL_AUDIO_PAUSED);
        QCOMPARE(SDL_GetQueuedAudioSize(id), Uint32{0});
        QCOMPARE(former_audio.size(), 2);
        QCOMPARE(former_audio.at(0).at(0).toBool(), true);
        QCOMPARE(former_audio.at(1).at(0).toBool(), false);
        current.set_audible(id, obtained.samples * 4);
        QVERIFY(current.audible());
        QCOMPARE(SDL_QueueAudio(id, pcm.constData(), static_cast<Uint32>(pcm.size())), 0);
        QCOMPARE(SDL_GetQueuedAudioSize(id), static_cast<Uint32>(pcm.size()));
        if (playing)
        {
            SDL_PauseAudioDevice(id, 0);
            QCOMPARE(SDL_GetAudioDeviceStatus(id), SDL_AUDIO_PLAYING);
        }

        if (cleanup == QStringLiteral("destroy"))
        {
            former.reset();
        }
        else
        {
            former->stop();
            if (cleanup == QStringLiteral("repeated-stop"))
            {
                former->stop();
                former->stop();
            }
        }
        QVERIFY(current.audible());
        QCOMPARE(current_audio.size(), 1);
        QCOMPARE(SDL_GetAudioDeviceStatus(id), playing ? SDL_AUDIO_PLAYING : SDL_AUDIO_PAUSED);
        if (!playing)
        {
            // dummy 的消费线程只在 PLAYING 时运行；暂停分支可精确检查旧owner未清队列。
            QCOMPARE(SDL_GetQueuedAudioSize(id), static_cast<Uint32>(pcm.size()));
        }
        current.stop();
        current.stop();
        QVERIFY(!current.audible());
        QCOMPARE(current_audio.size(), 2);
        QCOMPARE(SDL_GetAudioDeviceStatus(id), SDL_AUDIO_PAUSED);
        QCOMPARE(SDL_GetQueuedAudioSize(id), Uint32{0});
        // device 仍由测试owner持有，最后才由 close_audio 关闭。
        QCOMPARE(SDL_QueueAudio(id, pcm.constData(), static_cast<Uint32>(pcm.size())), 0);
        QCOMPARE(SDL_GetQueuedAudioSize(id), static_cast<Uint32>(pcm.size()));
    }

    void late_finished_keeps_new_audio_lease()
    {
        QVERIFY(SDL_WasInit(0) == 0);
        const bool had_driver = qEnvironmentVariableIsSet("SDL_AUDIODRIVER");
        const auto previous_driver = qgetenv("SDL_AUDIODRIVER");
        const auto restore_driver = qScopeGuard(
            [&]()
            {
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
        const auto quit_audio = qScopeGuard([]() { SDL_Quit(); });
        QCOMPARE(QByteArray(SDL_GetCurrentAudioDriver()), QByteArray("dummy"));
        SDL_AudioSpec wanted{};
        wanted.freq = 48'000;
        wanted.format = AUDIO_S16SYS;
        wanted.channels = 2;
        wanted.samples = 512;
        SDL_AudioSpec obtained{};
        const auto id = SDL_OpenAudioDevice(nullptr, 0, &wanted, &obtained, 0);
        QVERIFY2(id != 0, SDL_GetError());
        const auto close_audio = qScopeGuard([&]() { SDL_CloseAudioDevice(id); });
        QTcpServer api;
        QTcpServer rtsp;
        QVERIFY(api.listen(QHostAddress::LocalHost, 0));
        QVERIFY(rtsp.listen(QHostAddress::LocalHost, 0));
        platform_client client;
        QVERIFY(client.set_base_url(QUrl(QString("http://127.0.0.1:%1").arg(api.serverPort()))));
        auto former = std::make_unique<playback_view>();
        playback_view current;
        const auto cleanup = qScopeGuard(
            [&]()
            {
                if (former)
                {
                    former->stop();
                }
                current.stop();
                for (auto* thread : QCoreApplication::instance()->findChildren<decode_thread*>())
                {
                    thread->requestInterruption();
                    if (!thread->wait(3000))
                    {
                        qFatal("借用音频设备验证后解码线程未退出");
                    }
                }
            });
        former->play(client, target(QStringLiteral("former")));
        QTRY_VERIFY_WITH_TIMEOUT(api.hasPendingConnections(), 2000);
        auto* api_peer = api.nextPendingConnection();
        QVERIFY(api_peer);
        QTRY_VERIFY_WITH_TIMEOUT(complete_request(api_peer), 1000);
        QVERIFY(respond(api_peer, 200, {{"rtsp_url", QString("rtsp://127.0.0.1:%1/hang").arg(rtsp.serverPort())}}) > 0);
        QTRY_VERIFY_WITH_TIMEOUT(rtsp.hasPendingConnections(), 2000);
        auto* rtsp_peer = rtsp.nextPendingConnection();
        QVERIFY(rtsp_peer);
        QTRY_VERIFY_WITH_TIMEOUT(rtsp_peer->peek(65536).contains("\r\n\r\n"), 1000);
        QVERIFY(rtsp_peer->peek(65536).startsWith("OPTIONS rtsp://"));
        const auto decoders = QCoreApplication::instance()->findChildren<decode_thread*>();
        QCOMPARE(decoders.size(), 1);
        QPointer<decode_thread> decoder = decoders.front();
        QSignalSpy finished(decoder, &QThread::finished);
        QSignalSpy destroyed(decoder, &QObject::destroyed);
        QSignalSpy former_status(former.get(), &playback_view::status_changed);
        QSignalSpy current_audio(&current, &playback_view::audible_changed);
        const QByteArray pcm(19'200, '\x42');
        former->set_audible(id, obtained.samples * 4);
        QCOMPARE(SDL_QueueAudio(id, pcm.constData(), static_cast<Uint32>(pcm.size())), 0);
        former->set_audible(0, 0);
        QCOMPARE(SDL_GetQueuedAudioSize(id), Uint32{0});
        current.set_audible(id, obtained.samples * 4);
        QCOMPARE(SDL_QueueAudio(id, pcm.constData(), static_cast<Uint32>(pcm.size())), 0);
        QCOMPARE(SDL_GetQueuedAudioSize(id), static_cast<Uint32>(pcm.size()));

        // 不stop旧view：真实RTSP EOF让其现存session的finished回调执行。
        rtsp_peer->disconnectFromHost();
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 2000);
        QTRY_COMPARE_WITH_TIMEOUT(destroyed.size(), 1, 1000);
        QVERIFY(decoder.isNull());
        QTRY_VERIFY_WITH_TIMEOUT(former->snapshot().ended, 1000);
        QVERIFY(!former_status.isEmpty());
        QVERIFY(!former->audible());
        QVERIFY(current.audible());
        QCOMPARE(current_audio.size(), 1);
        QCOMPARE(SDL_GetAudioDeviceStatus(id), SDL_AUDIO_PAUSED);
        QCOMPARE(SDL_GetQueuedAudioSize(id), static_cast<Uint32>(pcm.size()));
        former->stop();
        former.reset();
        QCOMPARE(SDL_GetAudioDeviceStatus(id), SDL_AUDIO_PAUSED);
        QCOMPARE(SDL_GetQueuedAudioSize(id), static_cast<Uint32>(pcm.size()));
        current.stop();
        QCOMPARE(SDL_GetAudioDeviceStatus(id), SDL_AUDIO_PAUSED);
        QCOMPARE(SDL_GetQueuedAudioSize(id), Uint32{0});
    }

    void stop_pending_request_data()
    {
        QTest::addColumn<int>("stop_count");
        QTest::newRow("stop-once") << 1;
        QTest::newRow("stop-repeatedly") << 3;
    }

    void stop_pending_request()
    {
        QFETCH(int, stop_count);
        QTcpServer api;
        QTcpServer rtsp;
        QVERIFY(api.listen(QHostAddress::LocalHost, 0));
        QVERIFY(rtsp.listen(QHostAddress::LocalHost, 0));
        platform_client client;
        QVERIFY(client.set_base_url(QUrl(QString("http://127.0.0.1:%1").arg(api.serverPort()))));
        playback_view view;
        QVERIFY(!view.audible());
        QSignalSpy status(&view, &playback_view::status_changed);
        QSignalSpy errors(&view, &playback_view::playback_error);
        view.play(client, target(QStringLiteral("pending")));
        QTRY_VERIFY_WITH_TIMEOUT(api.hasPendingConnections(), 2000);
        auto* peer = api.nextPendingConnection();
        QVERIFY(peer);
        QTRY_VERIFY_WITH_TIMEOUT(complete_request(peer), 1000);
        QVERIFY(peer->peek(65536).startsWith("POST /api/play HTTP/1.1\r\n"));
        const auto replies = client.findChildren<QNetworkReply*>();
        QCOMPARE(replies.size(), 1);
        QPointer<QNetworkReply> reply = replies.front();
        QSignalSpy finished(reply, &QNetworkReply::finished);
        QSignalSpy destroyed(reply, &QObject::destroyed);
        QVERIFY(!reply->isFinished());

        // 回复已交给真实 TCP；在 GUI 分发完成前关闭，覆盖迟到成功及 abort 完成。
        QVERIFY(respond(peer, 200, {{"rtsp_url", QString("rtsp://127.0.0.1:%1/late").arg(rtsp.serverPort())}}) > 0);
        peer->flush();
        status.clear();
        for (int index = 0; index < stop_count; ++index)
        {
            view.stop();
        }
        QCOMPARE(view.status(), QStringLiteral("已停止"));
        QCOMPARE(status.size(), stop_count);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 1000);
        QTRY_COMPARE_WITH_TIMEOUT(destroyed.size(), 1, 1000);
        QVERIFY(reply.isNull());
        QTRY_COMPARE_WITH_TIMEOUT(peer->state(), QAbstractSocket::UnconnectedState, 1000);
        QCOMPARE(view.status(), QStringLiteral("已停止"));
        QCOMPARE(status.size(), stop_count);
        QCOMPARE(errors.size(), 0);
        QCOMPARE(view.rendered_frames(), quint64(0));
        QVERIFY(!rtsp.hasPendingConnections());
        QVERIFY(QCoreApplication::instance()->findChildren<decode_thread*>().isEmpty());
    }

    void replace_pending_request()
    {
        QTcpServer api;
        QVERIFY(api.listen(QHostAddress::LocalHost, 0));
        platform_client client;
        QVERIFY(client.set_base_url(QUrl(QString("http://127.0.0.1:%1").arg(api.serverPort()))));
        playback_view view;
        QVERIFY(!view.audible());
        QSignalSpy status(&view, &playback_view::status_changed);
        view.play(client, target(QStringLiteral("old")));
        QTRY_VERIFY_WITH_TIMEOUT(api.hasPendingConnections(), 2000);
        auto* old_peer = api.nextPendingConnection();
        QVERIFY(old_peer);
        QTRY_VERIFY_WITH_TIMEOUT(complete_request(old_peer), 1000);
        const auto replies = client.findChildren<QNetworkReply*>();
        QCOMPARE(replies.size(), 1);
        QPointer<QNetworkReply> old_reply = replies.front();
        QSignalSpy old_finished(old_reply, &QNetworkReply::finished);
        QSignalSpy old_destroyed(old_reply, &QObject::destroyed);
        QVERIFY(!old_reply->isFinished());
        QVERIFY(respond(old_peer, 500, {{"error", "old-error"}}) > 0);
        old_peer->flush();
        status.clear();
        view.play(client, target(QStringLiteral("new")));
        const auto pending = QStringLiteral("new · 正在申请播放");
        QCOMPARE(view.status(), pending);
        QTRY_COMPARE_WITH_TIMEOUT(old_finished.size(), 1, 1000);
        QTRY_COMPARE_WITH_TIMEOUT(old_destroyed.size(), 1, 1000);
        QVERIFY(old_reply.isNull());
        QTRY_COMPARE_WITH_TIMEOUT(old_peer->state(), QAbstractSocket::UnconnectedState, 1000);
        QCOMPARE(view.status(), pending);
        QCOMPARE(status.size(), 2);
        QTRY_VERIFY_WITH_TIMEOUT(api.hasPendingConnections(), 2000);
        auto* new_peer = api.nextPendingConnection();
        QVERIFY(new_peer);
        QTRY_VERIFY_WITH_TIMEOUT(complete_request(new_peer), 1000);
        QVERIFY(new_peer->peek(65536).contains("\"source_id\":\"new\""));
        QVERIFY(respond(new_peer, 503, {{"error", "new-error"}}) > 0);
        QTRY_VERIFY_WITH_TIMEOUT(view.status().contains(QStringLiteral("new-error")), 1000);
        QCOMPARE(status.size(), 3);
        QVERIFY(!view.status().contains(QStringLiteral("old-error")));
        QVERIFY(QCoreApplication::instance()->findChildren<decode_thread*>().isEmpty());
    }

    void cancel_pending_readiness_data()
    {
        QTest::addColumn<QString>("action");
        QTest::newRow("stop") << QStringLiteral("stop");
        QTest::newRow("replace") << QStringLiteral("replace");
        QTest::newRow("destroy") << QStringLiteral("destroy");
    }

    void cancel_pending_readiness()
    {
        QFETCH(QString, action);
        QTcpServer api;
        QTcpServer rtsp;
        QVERIFY(api.listen(QHostAddress::LocalHost, 0));
        QVERIFY(rtsp.listen(QHostAddress::LocalHost, 0));
        platform_client client;
        QVERIFY(client.set_base_url(QUrl(QString("http://127.0.0.1:%1").arg(api.serverPort()))));
        auto view = std::make_unique<playback_view>();
        const auto cleanup = qScopeGuard(
            [&]()
            {
                view->stop();
                for (auto* thread : QCoreApplication::instance()->findChildren<decode_thread*>())
                {
                    thread->requestInterruption();
                    if (!thread->wait(3000))
                    {
                        qFatal("取消就绪查询后解码线程未退出");
                    }
                }
            });
        view->play(client, {play_target::kind::gb_channel, "device", "channel", "gb old"});
        QTRY_VERIFY_WITH_TIMEOUT(api.hasPendingConnections(), 2000);
        auto* ticket_peer = api.nextPendingConnection();
        QVERIFY(ticket_peer);
        QTRY_VERIFY_WITH_TIMEOUT(complete_request(ticket_peer), 1000);
        QVERIFY(ticket_peer->peek(65536).startsWith("POST /api/devices/device/channels/channel/play HTTP/1.1\r\n"));
        QVERIFY(respond(ticket_peer, 201, {{"stream_id", "live"}, {"rtsp_url", QString("rtsp://127.0.0.1:%1/gb").arg(rtsp.serverPort())}}) > 0);
        QTRY_VERIFY_WITH_TIMEOUT(api.hasPendingConnections(), 2000);
        auto* readiness_peer = api.nextPendingConnection();
        QVERIFY(readiness_peer);
        QTRY_VERIFY_WITH_TIMEOUT(complete_request(readiness_peer), 1000);
        QVERIFY(readiness_peer->peek(65536).startsWith("GET /api/devices/device/channels HTTP/1.1\r\n"));
        QPointer<QNetworkReply> reply;
        for (auto* candidate : client.findChildren<QNetworkReply*>())
        {
            if (!candidate->isFinished())
            {
                QVERIFY(reply.isNull());
                reply = candidate;
            }
        }
        QVERIFY(reply);
        QSignalSpy finished(reply, &QNetworkReply::finished);
        QSignalSpy destroyed(reply, &QObject::destroyed);
        QVERIFY(!rtsp.hasPendingConnections());
        QVERIFY(QCoreApplication::instance()->findChildren<decode_thread*>().isEmpty());
        QVERIFY(respond(readiness_peer, 200, channels("live", true)) > 0);
        readiness_peer->flush();
        QPointer<playback_view> old_view = view.get();
        if (action == QStringLiteral("destroy"))
        {
            view = std::make_unique<playback_view>();
            QVERIFY(old_view.isNull());
        }
        else
        {
            view->stop();
            view->stop();
            QCOMPARE(view->status(), QStringLiteral("已停止"));
        }
        if (action != QStringLiteral("stop"))
        {
            view->play(client, target(QStringLiteral("new")));
        }
        const auto expected = action == QStringLiteral("stop") ? QStringLiteral("已停止") : QStringLiteral("new · 正在申请播放");
        const auto status_before = view->status();
        QCOMPARE(status_before, expected);
        QSignalSpy status(view.get(), &playback_view::status_changed);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 1000);
        QTRY_COMPARE_WITH_TIMEOUT(destroyed.size(), 1, 1000);
        QVERIFY(reply.isNull());
        QTRY_COMPARE_WITH_TIMEOUT(readiness_peer->state(), QAbstractSocket::UnconnectedState, 1000);
        QCOMPARE(view->status(), expected);
        QCOMPARE(status.size(), 0);
        QVERIFY(!rtsp.hasPendingConnections());
        QVERIFY(QCoreApplication::instance()->findChildren<decode_thread*>().isEmpty());
        if (action != QStringLiteral("stop"))
        {
            QTRY_VERIFY_WITH_TIMEOUT(api.hasPendingConnections(), 2000);
            auto* new_peer = api.nextPendingConnection();
            QVERIFY(new_peer);
            QTRY_VERIFY_WITH_TIMEOUT(complete_request(new_peer), 1000);
            QVERIFY(new_peer->peek(65536).startsWith("POST /api/play HTTP/1.1\r\n"));
            QVERIFY(respond(new_peer, 503, {{"error", "new-error"}}) > 0);
            QTRY_VERIFY_WITH_TIMEOUT(view->status().contains(QStringLiteral("new-error")), 1000);
        }
    }

    void gb_readiness_data()
    {
        QTest::addColumn<QString>("outcome");
        QTest::newRow("same-generation-ready") << QStringLiteral("ready");
        QTest::newRow("different-generation") << QStringLiteral("generation");
        QTest::newRow("http-failure") << QStringLiteral("http");
        QTest::newRow("missing-live") << QStringLiteral("missing");
        QTest::newRow("not-streaming") << QStringLiteral("state");
        QTest::newRow("missing-readiness-field") << QStringLiteral("missing-ready");
    }

    void gb_readiness()
    {
        QFETCH(QString, outcome);
        QTcpServer api;
        QTcpServer rtsp;
        QVERIFY(api.listen(QHostAddress::LocalHost, 0));
        QVERIFY(rtsp.listen(QHostAddress::LocalHost, 0));
        platform_client client;
        QVERIFY(client.set_base_url(QUrl(QString("http://127.0.0.1:%1").arg(api.serverPort()))));
        playback_view view;
        const auto cleanup = qScopeGuard(
            [&]()
            {
                view.stop();
                for (auto* thread : QCoreApplication::instance()->findChildren<decode_thread*>())
                {
                    thread->requestInterruption();
                    if (!thread->wait(3000))
                    {
                        qFatal("就绪验证后解码线程未退出");
                    }
                }
            });
        view.play(client, {play_target::kind::gb_channel, "device", "channel", "gb"});
        QTRY_VERIFY_WITH_TIMEOUT(api.hasPendingConnections(), 2000);
        auto* ticket_peer = api.nextPendingConnection();
        QVERIFY(ticket_peer);
        QTRY_VERIFY_WITH_TIMEOUT(complete_request(ticket_peer), 1000);
        QVERIFY(respond(ticket_peer, 201, {{"stream_id", "live"}, {"rtsp_url", QString("rtsp://127.0.0.1:%1/gb").arg(rtsp.serverPort())}}) > 0);
        QTRY_VERIFY_WITH_TIMEOUT(api.hasPendingConnections(), 2000);
        auto* first_peer = api.nextPendingConnection();
        QVERIFY(first_peer);
        QTRY_VERIFY_WITH_TIMEOUT(complete_request(first_peer), 1000);
        QVERIFY(first_peer->peek(65536).startsWith("GET /api/devices/device/channels HTTP/1.1\r\n"));
        QVERIFY(!rtsp.hasPendingConnections());
        QVERIFY(QCoreApplication::instance()->findChildren<decode_thread*>().isEmpty());
        QElapsedTimer polling;
        polling.start();
        // 另一通道已经 ready，不得绕过目标 channel 的 media_ready=false。
        QVERIFY(respond(first_peer, 200, channels("live", false)) > 0);
        QTRY_VERIFY_WITH_TIMEOUT(api.hasPendingConnections(), 2000);
        QVERIFY(polling.elapsed() >= 450);
        auto* next_peer = api.nextPendingConnection();
        QVERIFY(next_peer);
        QTRY_VERIFY_WITH_TIMEOUT(complete_request(next_peer), 1000);
        QVERIFY(next_peer->peek(65536).startsWith("GET /api/devices/device/channels HTTP/1.1\r\n"));
        QVERIFY(!rtsp.hasPendingConnections());
        QVERIFY(QCoreApplication::instance()->findChildren<decode_thread*>().isEmpty());
        const auto pending = view.status();
        QJsonObject document = channels(outcome == QStringLiteral("generation") ? QStringLiteral("new-live") : QStringLiteral("live"),
                                        true,
                                        outcome == QStringLiteral("state") ? QStringLiteral("stopped") : QStringLiteral("streaming"));
        if (outcome == QStringLiteral("missing"))
        {
            document = {{"channels", QJsonArray{QJsonObject{{"channel_id", "channel"}}}}};
        }
        if (outcome == QStringLiteral("missing-ready"))
        {
            document = {
                {"channels", QJsonArray{QJsonObject{{"channel_id", "channel"}, {"live", QJsonObject{{"live_id", "live"}, {"state", "streaming"}}}}}}};
        }
        const bool http_error = outcome == QStringLiteral("http");
        QVERIFY(respond(next_peer, http_error ? 503 : 200, http_error ? QJsonObject{{"error", "readiness-error"}} : document) > 0);
        if (outcome == QStringLiteral("ready"))
        {
            QTRY_VERIFY_WITH_TIMEOUT(rtsp.hasPendingConnections(), 2000);
            auto* rtsp_peer = rtsp.nextPendingConnection();
            QVERIFY(rtsp_peer);
            QTRY_VERIFY_WITH_TIMEOUT(rtsp_peer->peek(65536).contains("\r\n\r\n"), 1000);
            QVERIFY(rtsp_peer->peek(65536).startsWith("OPTIONS rtsp://"));
            const auto decoders = QCoreApplication::instance()->findChildren<decode_thread*>();
            QCOMPARE(decoders.size(), 1);
            QPointer<decode_thread> decoder = decoders.front();
            QSignalSpy destroyed(decoder, &QObject::destroyed);
            rtsp_peer->disconnectFromHost();
            QTRY_VERIFY_WITH_TIMEOUT(view.snapshot().ended, 2000);
            QTRY_COMPARE_WITH_TIMEOUT(destroyed.size(), 1, 1000);
            QVERIFY(decoder.isNull());
            // RTSP EOF 沿终态结束；不能重新申请票据或继续轮询。
            QVERIFY(!api.hasPendingConnections());
            QVERIFY(!rtsp.hasPendingConnections());
        }
        else
        {
            QTRY_VERIFY_WITH_TIMEOUT(view.status() != pending, 1000);
            const auto prefix = http_error                                   ? QStringLiteral("媒体就绪查询失败")
                                : outcome == QStringLiteral("missing-ready") ? QStringLiteral("平台缺少媒体就绪信息")
                                                                             : QStringLiteral("点播已停止或媒体源已变化");
            QVERIFY(view.status().startsWith(prefix));
            QTRY_COMPARE_WITH_TIMEOUT(next_peer->state(), QAbstractSocket::UnconnectedState, 1000);
            QVERIFY(!api.hasPendingConnections());
            QVERIFY(!rtsp.hasPendingConnections());
            QVERIFY(QCoreApplication::instance()->findChildren<decode_thread*>().isEmpty());
        }
        QVERIFY(!view.audible());
    }

    void gb_readiness_total_timeout()
    {
        QTcpServer api;
        QTcpServer rtsp;
        QVERIFY(api.listen(QHostAddress::LocalHost, 0));
        QVERIFY(rtsp.listen(QHostAddress::LocalHost, 0));
        platform_client client;
        QVERIFY(client.set_base_url(QUrl(QString("http://127.0.0.1:%1").arg(api.serverPort()))));
        playback_view view;
        int tickets = 0;
        int lookups = 0;
        bool valid_paths = true;
        QElapsedTimer deadline;
        connect(&api,
                &QTcpServer::newConnection,
                &api,
                [&]()
                {
                    while (api.hasPendingConnections())
                    {
                        auto* peer = api.nextPendingConnection();
                        connect(peer,
                                &QTcpSocket::readyRead,
                                &api,
                                [&, peer]()
                                {
                                    if (!complete_request(peer))
                                    {
                                        return;
                                    }
                                    const auto request = peer->readAll();
                                    if (request.startsWith("POST /api/devices/device/channels/channel/play HTTP/1.1\r\n"))
                                    {
                                        ++tickets;
                                        deadline.start();
                                        respond(peer,
                                                201,
                                                {{"stream_id", "live"}, {"rtsp_url", QString("rtsp://127.0.0.1:%1/gb").arg(rtsp.serverPort())}});
                                    }
                                    else
                                    {
                                        valid_paths = valid_paths && request.startsWith("GET /api/devices/device/channels HTTP/1.1\r\n");
                                        ++lookups;
                                        respond(peer, 200, channels("live", false));
                                    }
                                    peer->disconnectFromHost();
                                });
                    }
                });
        view.play(client, {play_target::kind::gb_channel, "device", "channel", "gb timeout"});
        QTRY_VERIFY_WITH_TIMEOUT(view.status().contains(QStringLiteral("未在15秒内就绪")), 18000);
        QVERIFY(deadline.isValid());
        QVERIFY(deadline.elapsed() >= 14500);
        QVERIFY(deadline.elapsed() < 18000);
        QVERIFY(valid_paths);
        QCOMPARE(tickets, 1);
        QVERIFY(lookups >= 20);
        QVERIFY(!rtsp.hasPendingConnections());
        QVERIFY(QCoreApplication::instance()->findChildren<decode_thread*>().isEmpty());
        QVERIFY(!view.audible());
    }

    void interrupt_rtsp_data()
    {
        QTest::addColumn<bool>("destroy_view");
        QTest::newRow("stop-and-reuse-view") << false;
        QTest::newRow("destroy-and-create-view") << true;
    }

    void interrupt_rtsp()
    {
        QFETCH(bool, destroy_view);
        QTcpServer api;
        QTcpServer rtsp;
        QVERIFY(api.listen(QHostAddress::LocalHost, 0));
        QVERIFY(rtsp.listen(QHostAddress::LocalHost, 0));
        platform_client client;
        QVERIFY(client.set_base_url(QUrl(QString("http://127.0.0.1:%1").arg(api.serverPort()))));
        auto view = std::make_unique<playback_view>();
        QVERIFY(!view->audible());
        QPointer<decode_thread> decoder;
        const auto cleanup = qScopeGuard(
            [&]()
            {
                view->stop();
                for (auto* thread : QCoreApplication::instance()->findChildren<decode_thread*>())
                {
                    thread->requestInterruption();
                    if (!thread->wait(3000))
                    {
                        qFatal("视图关闭后 RTSP 解码线程未退出");
                    }
                }
            });
        view->play(client, target(QStringLiteral("old")));
        QTRY_VERIFY_WITH_TIMEOUT(api.hasPendingConnections(), 2000);
        auto* api_peer = api.nextPendingConnection();
        QVERIFY(api_peer);
        QTRY_VERIFY_WITH_TIMEOUT(complete_request(api_peer), 1000);
        QVERIFY(respond(api_peer, 200, {{"rtsp_url", QString("rtsp://127.0.0.1:%1/hang").arg(rtsp.serverPort())}}) > 0);
        QTRY_VERIFY_WITH_TIMEOUT(rtsp.hasPendingConnections(), 2000);
        auto* rtsp_peer = rtsp.nextPendingConnection();
        QVERIFY(rtsp_peer);
        QTRY_VERIFY_WITH_TIMEOUT(rtsp_peer->peek(65536).contains("\r\n\r\n"), 1000);
        QVERIFY(rtsp_peer->peek(65536).startsWith("OPTIONS rtsp://"));
        const auto decoders = QCoreApplication::instance()->findChildren<decode_thread*>();
        QCOMPARE(decoders.size(), 1);
        decoder = decoders.front();
        QVERIFY(decoder->isRunning());
        QVERIFY(!decoder->isInterruptionRequested());
        QSignalSpy finished(decoder, &QThread::finished);
        QSignalSpy destroyed(decoder, &QObject::destroyed);
        QPointer<playback_view> old_view = view.get();
        if (destroy_view)
        {
            view = std::make_unique<playback_view>();
            QVERIFY(old_view.isNull());
        }
        else
        {
            view->stop();
            view->stop();
            QCOMPARE(view->status(), QStringLiteral("已停止"));
        }
        QVERIFY(decoder);
        QVERIFY(decoder->isInterruptionRequested());
        QSignalSpy status(view.get(), &playback_view::status_changed);
        view->play(client, target(QStringLiteral("new")));
        const auto pending = QStringLiteral("new · 正在申请播放");
        QCOMPARE(view->status(), pending);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 2000);
        QTRY_COMPARE_WITH_TIMEOUT(destroyed.size(), 1, 1000);
        QVERIFY(decoder.isNull());
        QTRY_COMPARE_WITH_TIMEOUT(rtsp_peer->state(), QAbstractSocket::UnconnectedState, 1000);
        QCOMPARE(view->status(), pending);
        QCOMPARE(status.size(), 2);
        QCOMPARE(view->snapshot().video_frames, quint64(0));
        QVERIFY(!view->snapshot().ended);
        QVERIFY(view->snapshot().error.isEmpty());
        QVERIFY(!view->audible());
        QTRY_VERIFY_WITH_TIMEOUT(api.hasPendingConnections(), 2000);
        auto* new_peer = api.nextPendingConnection();
        QVERIFY(new_peer);
        QTRY_VERIFY_WITH_TIMEOUT(complete_request(new_peer), 1000);
        QVERIFY(respond(new_peer, 503, {{"error", "new-error"}}) > 0);
        QTRY_VERIFY_WITH_TIMEOUT(view->status().contains(QStringLiteral("new-error")), 1000);
        QCOMPARE(status.size(), 3);
        QVERIFY(QCoreApplication::instance()->findChildren<decode_thread*>().isEmpty());
    }

    void live_recording_data()
    {
        QTest::addColumn<bool>("stop_before_ticket");
        QTest::newRow("recorder-started") << false;
        QTest::newRow("stop-before-ticket") << true;
    }

    // 本地录制另签独立票据；未收到视频的停止不留文件；画面停止后迟到票据不能启动录制。
    void live_recording()
    {
        QFETCH(bool, stop_before_ticket);
        QTcpServer api;
        QTcpServer rtsp;
        QVERIFY(api.listen(QHostAddress::LocalHost, 0));
        QVERIFY(rtsp.listen(QHostAddress::LocalHost, 0));
        platform_client client;
        QVERIFY(client.set_base_url(QUrl(QString("http://127.0.0.1:%1").arg(api.serverPort()))));
        QTemporaryDir directory;
        playback_view view;
        QSignalSpy finished(&view, &playback_view::recording_finished);
        const auto cleanup = qScopeGuard(
            [&]()
            {
                view.stop();
                for (auto* thread : QCoreApplication::instance()->findChildren<QThread*>())
                {
                    thread->requestInterruption();
                    thread->wait(5000);
                }
            });
        QVERIFY(!view.recording_ready());
        view.play(client, target(QStringLiteral("../录制/源")));
        QTRY_VERIFY_WITH_TIMEOUT(api.hasPendingConnections(), 2000);
        auto* play_peer = api.nextPendingConnection();
        QTRY_VERIFY_WITH_TIMEOUT(complete_request(play_peer), 1000);
        const auto play_request = play_peer->readAll();
        QVERIFY(respond(play_peer, 200, {{"rtsp_url", QString("rtsp://127.0.0.1:%1/hang").arg(rtsp.serverPort())}}) > 0);
        QTRY_VERIFY_WITH_TIMEOUT(rtsp.hasPendingConnections(), 2000);
        QVERIFY(rtsp.nextPendingConnection());
        QTRY_VERIFY_WITH_TIMEOUT(view.recording_ready(), 1000);
        view.start_recording(client, directory.path());
        QVERIFY(view.recording() && !view.recording_ready());
        QTRY_VERIFY_WITH_TIMEOUT(api.hasPendingConnections(), 2000);
        auto* record_peer = api.nextPendingConnection();
        QTRY_VERIFY_WITH_TIMEOUT(complete_request(record_peer), 1000);
        // 与观看相同的签票接口与请求体，但是另一张独立票据。
        QCOMPARE(record_peer->readAll().split('\n').first(), play_request.split('\n').first());
        if (stop_before_ticket)
        {
            view.stop();
            QVERIFY(!view.recording());
        }
        QVERIFY(respond(record_peer, 200, {{"rtsp_url", QString("rtsp://127.0.0.1:%1/record").arg(rtsp.serverPort())}}) > 0);
        if (stop_before_ticket)
        {
            QTest::qWait(300);
            QVERIFY(!rtsp.hasPendingConnections());
            QVERIFY(QCoreApplication::instance()->findChildren<record_thread*>().isEmpty());
            QCOMPARE(finished.size(), 0);
        }
        else
        {
            QTRY_VERIFY_WITH_TIMEOUT(rtsp.hasPendingConnections(), 2000);
            auto* record_rtsp = rtsp.nextPendingConnection();
            QTRY_VERIFY_WITH_TIMEOUT(record_rtsp->peek(65536).startsWith("OPTIONS rtsp://127.0.0.1"), 1000);
            QCOMPARE(QCoreApplication::instance()->findChildren<record_thread*>().size(), 1);
            view.stop_recording();
            QVERIFY(!view.recording());
            QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 3000);
            QVERIFY2(finished[0][0].toString().contains(QStringLiteral("录制在收到视频前停止")), qPrintable(finished[0][0].toString()));
            QTRY_VERIFY_WITH_TIMEOUT(QCoreApplication::instance()->findChildren<record_thread*>().isEmpty(), 3000);
        }
        QVERIFY(QDir(directory.path()).entryList(QDir::Files | QDir::Hidden).isEmpty());
    }
};

}    // namespace media_server

QTEST_MAIN(media_server::playback_view_test)
#include "playback_view_test.moc"
