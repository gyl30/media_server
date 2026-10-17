#include <SDL.h>
#include <QTest>
#include <QLineEdit>
#include <QSettings>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QJsonObject>
#include <QJsonArray>
#include <QPushButton>
#include <QTreeWidget>
#include <QTemporaryDir>
#include <QWebSocketServer>

#include "desktop/talk_session.h"
#include "desktop/monitor_window.h"

using namespace media_server;

namespace
{

class talk_fixture final : public QObject
{
   public:
    talk_fixture() : upload(QStringLiteral("audio-test"), QWebSocketServer::NonSecureMode)
    {
        connect(&api,
                &QTcpServer::newConnection,
                this,
                [this]()
                {
                    while (api.hasPendingConnections())
                    {
                        auto* connection = api.nextPendingConnection();
                        connect(connection, &QTcpSocket::disconnected, connection, &QObject::deleteLater);
                        connect(connection,
                                &QTcpSocket::readyRead,
                                this,
                                [this, connection]()
                                {
                                    auto data = connection->property("request").toByteArray() + connection->readAll();
                                    connection->setProperty("request", data);
                                    const auto end = data.indexOf("\r\n\r\n");
                                    if (end < 0 || connection->property("answered").toBool())
                                    {
                                        return;
                                    }
                                    int length = 0;
                                    for (const auto& line : data.left(end).split('\n'))
                                    {
                                        if (line.toLower().startsWith("content-length:"))
                                        {
                                            length = line.mid(15).trimmed().toInt();
                                        }
                                    }
                                    if (data.size() < end + 4 + length)
                                    {
                                        return;
                                    }
                                    connection->setProperty("answered", true);
                                    if (data.startsWith("GET "))
                                    {
                                        QJsonObject body;
                                        if (data.startsWith("GET /api/devices "))
                                        {
                                            body.insert(QStringLiteral("devices"),
                                                        QJsonArray{QJsonObject{{QStringLiteral("device_id"), QStringLiteral("34020000001320000089")},
                                                                               {QStringLiteral("name"), QStringLiteral("音频测试设备")},
                                                                               {QStringLiteral("online"), true}}});
                                        }
                                        const auto bytes = QJsonDocument(body).toJson();
                                        connection->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                                                          QByteArray::number(bytes.size()) + "\r\nConnection: close\r\n\r\n" + bytes);
                                        connection->disconnectFromHost();
                                        return;
                                    }
                                    if (data.startsWith("POST "))
                                    {
                                        ++creates;
                                        pending = connection;
                                        if (!hold)
                                        {
                                            reply_create();
                                        }
                                        return;
                                    }
                                    deletes.append(QJsonDocument::fromJson(data.mid(end + 4)).object().value(QStringLiteral("talk_id")).toString());
                                    connection->write("HTTP/1.1 204 No Content\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                                    connection->disconnectFromHost();
                                });
                    }
                });
        connect(&upload,
                &QWebSocketServer::newConnection,
                this,
                [this]()
                {
                    peer = upload.nextPendingConnection();
                    ++connections;
                    connect(peer, &QWebSocket::binaryMessageReceived, this, [this](const QByteArray& data) { packets.append(data); });
                    connect(peer, &QWebSocket::disconnected, peer, &QObject::deleteLater);
                });
    }

    void reply_create()
    {
        const auto body = QJsonDocument(QJsonObject{{QStringLiteral("talk_id"), id}, {QStringLiteral("upload_url"), upload_url}}).toJson();
        pending->write("HTTP/1.1 201 Created\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(body.size()) +
                       "\r\nConnection: close\r\n\r\n" + body);
        pending->disconnectFromHost();
    }

    bool listen()
    {
        if (!api.listen(QHostAddress::LocalHost) || !upload.listen(QHostAddress::LocalHost))
        {
            return false;
        }
        upload_url = QStringLiteral("ws://127.0.0.1:%1/publish/gb28181/audio/%2/%3").arg(upload.serverPort()).arg(id, QString(64, QLatin1Char('b')));
        return true;
    }

    QUrl base() const { return QUrl(QStringLiteral("http://127.0.0.1:%1").arg(api.serverPort())); }

    QTcpServer api;
    QWebSocketServer upload;
    QPointer<QTcpSocket> pending;
    QPointer<QWebSocket> peer;
    QString id{64, QLatin1Char('a')};
    QString upload_url;
    QList<QByteArray> packets;
    QStringList deletes;
    bool hold{};
    int creates{};
    int connections{};
};

class talk_session_test final : public QObject
{
    Q_OBJECT

   private slots:
    void initTestCase()
    {
        QCoreApplication::setOrganizationName(QStringLiteral("talk-session-test"));
        QCoreApplication::setApplicationName(QStringLiteral("talk-session-test"));
        QCOMPARE(SDL_InitSubSystem(SDL_INIT_AUDIO), 0);
    }
    void cleanupTestCase() { SDL_QuitSubSystem(SDL_INIT_AUDIO); }

    void capture_encode_upload_and_stop()
    {
        talk_fixture fixture;
        QVERIFY(fixture.listen());
        talk_session session(fixture.base(), QStringLiteral("34020000001320000089"));
        QSignalSpy ready(&session, &talk_session::transmitting);
        QSignalSpy done(&session, &talk_session::finished);
        session.start();
        QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 2000);
        QTRY_VERIFY_WITH_TIMEOUT(fixture.packets.size() >= 20, 3000);
        for (const auto& packet : fixture.packets)
        {
            QCOMPARE(packet, QByteArray(160, char(0xd5)));
        }
        session.stop();
        session.stop();
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 2000);
        QCOMPARE(fixture.deletes, QStringList{fixture.id});
        QVERIFY(done.front().front().toString().isEmpty());
        QCOMPARE(fixture.connections, 1);
        session.stop();
        QCOMPARE(done.size(), 1);
    }

    void cancel_pending_create()
    {
        talk_fixture fixture;
        fixture.hold = true;
        QVERIFY(fixture.listen());
        talk_session session(fixture.base(), QStringLiteral("34020000001320000089"));
        QSignalSpy done(&session, &talk_session::finished);
        session.start();
        QTRY_COMPARE_WITH_TIMEOUT(fixture.creates, 1, 2000);
        session.stop();
        session.stop();
        QCOMPARE(done.size(), 0);
        fixture.reply_create();
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 2000);
        QCOMPARE(fixture.connections, 0);
        QCOMPARE(fixture.deletes, QStringList{fixture.id});
    }

    void gui_stall_does_not_burst_audio()
    {
        talk_fixture fixture;
        QVERIFY(fixture.listen());
        talk_session session(fixture.base(), QStringLiteral("34020000001320000089"));
        QSignalSpy progress(&session, &talk_session::progress);
        QSignalSpy done(&session, &talk_session::finished);
        session.start();
        QTRY_VERIFY_WITH_TIMEOUT(progress.size() >= 10, 2000);
        const auto before = progress.size();
        // 故意阻塞 GUI；SDL callback 继续运行，验证恢复时不补发整段积压。
        QTest::qSleep(400);
        QCOMPARE(progress.size(), before);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
        QVERIFY(progress.size() <= before + 2);
        QTRY_VERIFY_WITH_TIMEOUT(progress.size() >= before + 5, 1000);
        session.stop();
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 2000);
        QCOMPARE(fixture.deletes, QStringList{fixture.id});
    }

    void remote_close()
    {
        talk_fixture fixture;
        QVERIFY(fixture.listen());
        talk_session session(fixture.base(), QStringLiteral("34020000001320000089"));
        QSignalSpy done(&session, &talk_session::finished);
        session.start();
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.packets.isEmpty(), 2000);
        fixture.peer->close();
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 2000);
        QCOMPARE(fixture.deletes, QStringList{fixture.id});
        QVERIFY(!done.front().front().toString().isEmpty());
    }

    void handshake_timeout()
    {
        talk_fixture fixture;
        QVERIFY(fixture.listen());
        QTcpServer blackhole;
        QVERIFY(blackhole.listen(QHostAddress::LocalHost));
        fixture.upload_url.replace(QString::number(fixture.upload.serverPort()), QString::number(blackhole.serverPort()));
        talk_session session(fixture.base(), QStringLiteral("34020000001320000089"));
        QSignalSpy done(&session, &talk_session::finished);
        session.start();
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 7000);
        QCOMPARE(fixture.deletes, QStringList{fixture.id});
        QVERIFY(done.front().front().toString().contains(QStringLiteral("超时")));
    }

    void invalid_upload()
    {
        talk_fixture fixture;
        QVERIFY(fixture.listen());
        fixture.upload_url += QStringLiteral("?unexpected=1");
        talk_session session(fixture.base(), QStringLiteral("34020000001320000089"));
        QSignalSpy done(&session, &talk_session::finished);
        session.start();
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 2000);
        QCOMPARE(fixture.deletes, QStringList{fixture.id});
        QCOMPARE(fixture.connections, 0);
    }

    void invalid_device()
    {
        talk_fixture fixture;
        QVERIFY(fixture.listen());
        talk_session session(fixture.base(), QStringLiteral("invalid"));
        QSignalSpy done(&session, &talk_session::finished);
        session.start();
        QCOMPARE(done.size(), 1);
        QCOMPARE(fixture.creates, 0);
    }

    void window_close_waits_for_pending_create()
    {
        QTemporaryDir settings;
        QVERIFY(settings.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
        talk_fixture fixture;
        fixture.hold = true;
        QVERIFY(fixture.listen());
        QSettings().setValue(QStringLiteral("platform_url"), fixture.base().toString());
        monitor_window window;
        window.show();
        auto* tree = window.findChild<QTreeWidget*>(QStringLiteral("device_tree"));
        auto* button = window.findChild<QPushButton*>(QStringLiteral("talk_device"));
        QVERIFY(tree && button);
        QTRY_VERIFY_WITH_TIMEOUT(tree->topLevelItem(0) && tree->topLevelItem(0)->childCount() == 1, 2000);
        tree->setCurrentItem(tree->topLevelItem(0)->child(0));
        QVERIFY(button->isEnabled());
        button->click();
        QTRY_COMPARE_WITH_TIMEOUT(fixture.creates, 1, 2000);
        QPointer<talk_session> owner = window.findChild<talk_session*>();
        QVERIFY(owner);
        QVERIFY(!window.close());
        QVERIFY(window.isVisible());
        fixture.reply_create();
        QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(owner.isNull(), 2000);
        QCOMPARE(fixture.connections, 0);
        QCOMPARE(fixture.deletes, QStringList{fixture.id});
    }

    void platform_switch_cleans_original_owner()
    {
        QTemporaryDir settings;
        QVERIFY(settings.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
        talk_fixture original;
        original.hold = true;
        talk_fixture replacement;
        QVERIFY(original.listen());
        QVERIFY(replacement.listen());
        QSettings().setValue(QStringLiteral("platform_url"), original.base().toString());
        monitor_window window;
        auto* tree = window.findChild<QTreeWidget*>(QStringLiteral("device_tree"));
        auto* button = window.findChild<QPushButton*>(QStringLiteral("talk_device"));
        QTRY_VERIFY_WITH_TIMEOUT(tree->topLevelItem(0) && tree->topLevelItem(0)->childCount() == 1, 2000);
        tree->setCurrentItem(tree->topLevelItem(0)->child(0));
        button->click();
        QTRY_COMPARE_WITH_TIMEOUT(original.creates, 1, 2000);
        window.findChild<QLineEdit*>(QStringLiteral("platform_address"))->setText(replacement.base().toString());
        window.findChild<QPushButton*>(QStringLiteral("refresh_devices"))->click();
        original.reply_create();
        QTRY_COMPARE_WITH_TIMEOUT(original.deletes.size(), 1, 2000);
        QTRY_VERIFY_WITH_TIMEOUT(window.findChild<talk_session*>() == nullptr, 2000);
        QVERIFY(replacement.deletes.isEmpty());
        QCOMPARE(original.connections, 0);
        QCOMPARE(original.deletes.front(), original.id);
    }
};

}    // namespace

QTEST_MAIN(talk_session_test)
#include "talk_session_test.moc"
