#include <QTest>
#include <QTimer>
#include <QVariant>
#include <QLineEdit>
#include <QSettings>
#include <QByteArray>
#include <QEventLoop>
#include <QTcpServer>
#include <QTcpSocket>
#include <QPushButton>
#include <QTreeWidget>
#include <QHostAddress>
#include <QTemporaryDir>

#include "desktop/playback_view.h"
#include "desktop/monitor_window.h"

using namespace media_server;

class monitor_window_test final : public QObject
{
    Q_OBJECT

   private slots:
    void changing_platform_cancels_pending_gb_playback()
    {
        QTemporaryDir settings;
        QVERIFY(settings.isValid());
        QCoreApplication::setOrganizationName(QStringLiteral("media-server-qt-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("monitor-window"));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
        QTcpServer first;
        QTcpServer second;
        QVERIFY(first.listen(QHostAddress::LocalHost, 0));
        QVERIFY(second.listen(QHostAddress::LocalHost, 0));
        int first_channels = 0;
        int second_channels = 0;
        int claims = 0;
        for (auto* server : {&first, &second})
        {
            connect(
                server,
                &QTcpServer::newConnection,
                this,
                [&, server]()
                {
                    while (server->hasPendingConnections())
                    {
                        auto* socket = server->nextPendingConnection();
                        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                        connect(
                            socket,
                            &QTcpSocket::readyRead,
                            socket,
                            [&, server, socket]()
                            {
                                auto bytes = socket->property("request").toByteArray() + socket->readAll();
                                socket->setProperty("request", bytes);
                                const auto end = bytes.indexOf("\r\n\r\n");
                                if (end < 0 || socket->property("answered").toBool())
                                {
                                    return;
                                }
                                int content_length = 0;
                                for (const auto& line : bytes.left(end).split('\n'))
                                {
                                    if (line.toLower().startsWith("content-length:"))
                                    {
                                        content_length = line.mid(15).trimmed().toInt();
                                    }
                                }
                                if (bytes.size() < end + 4 + content_length)
                                {
                                    return;
                                }
                                socket->setProperty("answered", true);
                                const auto path = bytes.split(' ').value(1);
                                QByteArray body = "{}";
                                if (path == "/api/devices")
                                {
                                    body = R"({"devices":[{"device_id":"34020000001320000001","name":"GB","online":true}]})";
                                }
                                else if (path == "/api/sources")
                                {
                                    body = R"({"sources":[]})";
                                }
                                else if (path == "/api/push-devices")
                                {
                                    body = R"({"devices":[]})";
                                }
                                else if (path.endsWith("/play"))
                                {
                                    ++claims;
                                    body = R"({"stream_id":"current-generation","rtsp_url":"rtsp://127.0.0.1:9/current/token"})";
                                }
                                else if (path.endsWith("/channels"))
                                {
                                    ++(server == &first ? first_channels : second_channels);
                                    body =
                                        R"({"channels":[{"channel_id":"34020000001310000001","name":"camera","live":{"live_id":"current-generation","state":"streaming","media_ready":false}}]})";
                                }
                                socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                                              QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                                socket->disconnectFromHost();
                            });
                    }
                });
        }
        QSettings().setValue(QStringLiteral("platform_url"), QStringLiteral("http://127.0.0.1:%1").arg(first.serverPort()));
        monitor_window window;
        auto* tree = window.findChild<QTreeWidget*>(QStringLiteral("device_tree"));
        auto* address = window.findChild<QLineEdit*>(QStringLiteral("platform_address"));
        auto* refresh = window.findChild<QPushButton*>(QStringLiteral("refresh_devices"));
        auto* view = window.findChild<playback_view*>();
        QVERIFY(tree && address && refresh && view);
        QTRY_VERIFY_WITH_TIMEOUT(tree->topLevelItemCount() == 3 && tree->topLevelItem(0)->childCount() == 1, 2000);
        auto* device = tree->topLevelItem(0)->child(0);
        tree->itemDoubleClicked(device, 0);
        QTRY_COMPARE_WITH_TIMEOUT(device->childCount(), 1, 2000);
        tree->itemDoubleClicked(device->child(0), 0);
        QTRY_COMPARE_WITH_TIMEOUT(claims, 1, 2000);
        QTRY_VERIFY_WITH_TIMEOUT(first_channels >= 2, 2000);
        QTRY_VERIFY_WITH_TIMEOUT(view->status().contains(QStringLiteral("等待国标媒体就绪")), 2000);

        // 无效输入不能清空当前连接配置，旧画面仍属于原平台。
        address->setText(QStringLiteral("not-a-platform-url"));
        refresh->click();
        QVERIFY(view->status().contains(QStringLiteral("等待国标媒体就绪")));
        const auto invalid_before = first_channels;
        QTRY_VERIFY_WITH_TIMEOUT(first_channels > invalid_before, 1500);
        QCOMPARE(claims, 1);

        // 同平台刷新只重建树，不能结束仍在等待的画面或重新认领票据。
        address->setText(QStringLiteral("http://127.0.0.1:%1").arg(first.serverPort()));
        refresh->click();
        QVERIFY(view->status().contains(QStringLiteral("等待国标媒体就绪")));
        const auto before = first_channels;
        QTRY_VERIFY_WITH_TIMEOUT(first_channels > before, 1500);
        QCOMPARE(claims, 1);

        address->setText(QStringLiteral("http://127.0.0.1:%1").arg(second.serverPort()));
        refresh->click();
        QCOMPARE(view->status(), QStringLiteral("已停止"));
        QEventLoop interval;
        QTimer::singleShot(700, &interval, &QEventLoop::quit);
        interval.exec();
        QCOMPARE(second_channels, 0);
        QCOMPARE(claims, 1);
        QCOMPARE(view->snapshot().video_frames, quint64{0});
        window.close();
    }
};

QTEST_MAIN(monitor_window_test)
#include "monitor_window_test.moc"
