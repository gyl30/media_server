#include <memory>

#include <QUrl>
#include <QTest>
#include <QString>
#include <QPointer>
#include <QByteArray>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
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
};

}    // namespace media_server

QTEST_MAIN(media_server::playback_view_test)
#include "playback_view_test.moc"
