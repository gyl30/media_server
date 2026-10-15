#include <memory>

#include <QMap>
#include <QTest>
#include <QList>
#include <QLabel>
#include <QTimer>
#include <QVariant>
#include <QPointer>
#include <QLineEdit>
#include <QComboBox>
#include <QCheckBox>
#include <QMimeData>
#include <QSettings>
#include <QIODevice>
#include <QByteArray>
#include <QDropEvent>
#include <QEventLoop>
#include <QTcpServer>
#include <QTcpSocket>
#include <QDataStream>
#include <QScopeGuard>
#include <QJsonObject>
#include <QPushButton>
#include <QTreeWidget>
#include <QHostAddress>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QTemporaryDir>
#include <QPlainTextEdit>
#include <QAbstractSocket>
#include <QDragEnterEvent>
#include <QCoreApplication>

#include "desktop/playback_view.h"
#include "desktop/monitor_window.h"

using namespace media_server;

namespace
{

void respond(QTcpSocket* socket, int status, const QByteArray& body)
{
    socket->write("HTTP/1.1 " + QByteArray::number(status) + " Test\r\nContent-Type: application/json\r\nContent-Length: " +
                  QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
    socket->disconnectFromHost();
}

// 原播放列表保持不变；查询用例另启用 GB 设备，回复由真实 TCP 屏障控制。
class platform_fixture final : public QObject
{
   public:
    struct pending_play
    {
        QPointer<QTcpSocket> socket;
        QString id;
    };

    struct pending_query
    {
        QPointer<QTcpSocket> socket;
        QByteArray request_line;
        QByteArray path;
        QJsonObject body;
    };

    explicit platform_fixture(bool gb_devices = false)
    {
        connect(
            &server,
            &QTcpServer::newConnection,
            this,
            [this, gb_devices]()
            {
                while (server.hasPendingConnections())
                {
                    auto* socket = server.nextPendingConnection();
                    connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                    connect(
                        socket,
                        &QTcpSocket::readyRead,
                        this,
                        [this, socket, gb_devices]()
                        {
                            const auto bytes = socket->property("request").toByteArray() + socket->readAll();
                            socket->setProperty("request", bytes);
                            const auto end = bytes.indexOf("\r\n\r\n");
                            if (end < 0 || socket->property("answered").toBool())
                            {
                                return;
                            }
                            int length = 0;
                            for (const auto& line : bytes.left(end).split('\n'))
                            {
                                if (line.toLower().startsWith("content-length:"))
                                {
                                    length = line.mid(15).trimmed().toInt();
                                }
                            }
                            if (bytes.size() < end + 4 + length)
                            {
                                return;
                            }
                            socket->setProperty("answered", true);
                            const auto path = bytes.split(' ').value(1);
                            if (path.endsWith("/query"))
                            {
                                queries.append(
                                    {socket, bytes.left(bytes.indexOf("\r\n")), path, QJsonDocument::fromJson(bytes.mid(end + 4, length)).object()});
                                return;
                            }
                            if (path == "/api/play")
                            {
                                const auto source = QJsonDocument::fromJson(bytes.mid(end + 4, length)).object().value("source").toObject();
                                plays.append({socket, source.value("push_device_id").toString()});
                                return;
                            }
                            QByteArray body = "{}";
                            if (path == "/api/devices")
                            {
                                body =
                                    gb_devices
                                        ? R"({"devices":[{"device_id":"34020000001320000001","name":"GB-A","online":true},{"device_id":"34020000001320000002","name":"GB-B","online":true}]})"
                                        : R"({"devices":[]})";
                            }
                            else if (path == "/api/sources")
                            {
                                body = gb_devices ? R"({"sources":[{"source_id":"source-a","stream_name":"active-a"}]})" : R"({"sources":[]})";
                            }
                            else if (path.endsWith("/channels"))
                            {
                                body = R"({"channels":[{"channel_id":"34020000001310000001","name":"channel-a"}]})";
                            }
                            else if (path == "/api/push-devices")
                            {
                                body =
                                    R"({"devices":[{"device_id":"a","name":"camera-a"},{"device_id":"b","name":"camera-b"},{"device_id":"c","name":"camera-c"}]})";
                            }
                            respond(socket, 200, body);
                        });
                }
            });
    }

    bool start()
    {
        if (!server.listen(QHostAddress::LocalHost, 0))
        {
            return false;
        }
        QSettings().setValue(QStringLiteral("platform_url"), QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()));
        return true;
    }

    QTcpServer server;
    QList<pending_play> plays;
    QList<pending_query> queries;
};

QByteArray device_query_response(const QString& id, const QString& command, const QString& marker, const QString& result = QStringLiteral("OK"))
{
    QJsonObject response{{"device_id", id}, {"command", command}, {"sn", 17}, {"result", result}};
    response.insert(command == QStringLiteral("DeviceInfo") ? QStringLiteral("info") : QStringLiteral("status"),
                    command == QStringLiteral("DeviceInfo") ? QJsonObject{{"device_name", marker}, {"manufacturer", "fixture-manufacturer"}}
                                                            : QJsonObject{{"online", "ONLINE"}, {"status", "OK"}, {"reason", marker}});
    return QJsonDocument(response).toJson(QJsonDocument::Compact);
}

QPointer<QNetworkReply> pending_device_query(monitor_window& window)
{
    for (auto* reply : window.findChildren<QNetworkReply*>())
    {
        if (!reply->isFinished() && reply->url().path().endsWith(QStringLiteral("/query")))
        {
            return reply;
        }
    }
    return {};
}

playback_view* slot_view(monitor_window& window, int slot)
{
    return window.findChild<playback_view*>(QStringLiteral("view_%1").arg(slot, 2, 10, QLatin1Char('0')));
}

QByteArray item_payload(const QVariantMap& data)
{
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream << 0 << 0 << QMap<int, QVariant>{{Qt::UserRole, data}};
    return bytes;
}

bool drop_item(playback_view* view, const QByteArray& payload, bool model_format = true)
{
    QMimeData mime;
    mime.setData(model_format ? QStringLiteral("application/x-qabstractitemmodeldatalist") : QStringLiteral("text/plain"), payload);
    QDragEnterEvent enter(QPoint(20, 20), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(view, &enter);
    QDropEvent drop(QPointF(20, 20), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(view, &drop);
    return enter.isAccepted() && drop.isAccepted();
}

}    // namespace

class monitor_window_test final : public QObject
{
    Q_OBJECT

   private slots:
    void init()
    {
        settings_ = std::make_unique<QTemporaryDir>();
        QVERIFY(settings_->isValid());
        QCoreApplication::setOrganizationName(QStringLiteral("media-server-qt-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("monitor-window-stage2"));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_->path());
    }

    void device_query_targets_and_serialization()
    {
        platform_fixture api(true);
        QVERIFY(api.start());
        monitor_window window;
        window.show();
        auto* tree = window.findChild<QTreeWidget*>(QStringLiteral("device_tree"));
        auto* info = window.findChild<QPushButton*>(QStringLiteral("device_info_query"));
        auto* status = window.findChild<QPushButton*>(QStringLiteral("device_status_query"));
        auto* result = window.findChild<QPlainTextEdit*>(QStringLiteral("device_query_result"));
        auto* target = window.findChild<QLabel*>(QStringLiteral("device_query_target"));
        QVERIFY(tree && info && status && result && target);
        QVERIFY(result->isReadOnly());
        QTRY_VERIFY_WITH_TIMEOUT(tree->topLevelItemCount() == 3 && tree->topLevelItem(0)->childCount() == 2 &&
                                     tree->topLevelItem(1)->childCount() == 1 && tree->topLevelItem(2)->childCount() == 3,
                                 2000);
        QVERIFY(!info->isEnabled() && !status->isEnabled());
        for (auto* item : {tree->topLevelItem(0), tree->topLevelItem(1)->child(0), tree->topLevelItem(2)->child(0)})
        {
            tree->setCurrentItem(item);
            QVERIFY(!info->isEnabled() && !status->isEnabled());
            info->click();
            status->click();
        }
        QCOMPARE(api.queries.size(), 0);
        auto* device = tree->topLevelItem(0)->child(0);
        tree->setCurrentItem(device);
        QVERIFY(info->isEnabled() && status->isEnabled());
        QVERIFY(target->text().contains(QStringLiteral("34020000001320000001")));
        info->click();
        QVERIFY(!info->isEnabled() && !status->isEnabled());
        status->click();
        QTRY_COMPARE_WITH_TIMEOUT(api.queries.size(), 1, 2000);
        QCOMPARE(api.queries[0].request_line, QByteArray("POST /api/devices/34020000001320000001/query HTTP/1.1"));
        QCOMPARE(api.queries[0].body, QJsonObject({{"command", "DeviceInfo"}}));
        const auto literal = QStringLiteral("<b>literal-device-name</b>");
        respond(api.queries[0].socket, 200, device_query_response(QStringLiteral("34020000001320000001"), QStringLiteral("DeviceInfo"), literal));
        QTRY_VERIFY_WITH_TIMEOUT(info->isEnabled() && status->isEnabled(), 1000);
        QVERIFY(result->toPlainText().contains(literal));
        QCOMPARE(api.queries.size(), 1);

        tree->itemDoubleClicked(device, 0);
        QTRY_COMPARE_WITH_TIMEOUT(device->childCount(), 1, 2000);
        tree->setCurrentItem(device->child(0));
        QVERIFY(info->isEnabled() && status->isEnabled());
        QVERIFY(result->toPlainText().isEmpty());
        status->click();
        QTRY_COMPARE_WITH_TIMEOUT(api.queries.size(), 2, 2000);
        QCOMPARE(api.queries[1].path, QByteArray("/api/devices/34020000001320000001/query"));
        QCOMPARE(api.queries[1].body, QJsonObject({{"command", "DeviceStatus"}}));
        respond(api.queries[1].socket,
                200,
                device_query_response(QStringLiteral("34020000001320000001"), QStringLiteral("DeviceStatus"), QStringLiteral("fresh-status")));
        QTRY_VERIFY_WITH_TIMEOUT(info->isEnabled() && status->isEnabled(), 1000);
        QVERIFY(result->toPlainText().contains(QStringLiteral("fresh-status")));
        QVERIFY(!result->toPlainText().contains(literal));
        tree->setCurrentItem(tree->topLevelItem(2)->child(0));
        QVERIFY(result->toPlainText().isEmpty());
        QVERIFY(!info->isEnabled() && !status->isEnabled());
        window.close();
    }

    void device_query_selection_isolates_late_reply_data()
    {
        QTest::addColumn<bool>("return_to_a");
        QTest::addColumn<int>("old_http_status");
        QTest::newRow("a-to-b-old-success") << false << 200;
        QTest::newRow("a-to-b-old-error") << false << 502;
        QTest::newRow("a-to-b-to-a-old-success") << true << 200;
    }

    void device_query_selection_isolates_late_reply()
    {
        QFETCH(bool, return_to_a);
        QFETCH(int, old_http_status);
        platform_fixture api(true);
        QVERIFY(api.start());
        monitor_window window;
        auto* tree = window.findChild<QTreeWidget*>(QStringLiteral("device_tree"));
        auto* info = window.findChild<QPushButton*>(QStringLiteral("device_info_query"));
        auto* status = window.findChild<QPushButton*>(QStringLiteral("device_status_query"));
        auto* result = window.findChild<QPlainTextEdit*>(QStringLiteral("device_query_result"));
        auto* target = window.findChild<QLabel*>(QStringLiteral("device_query_target"));
        QVERIFY(tree && info && status && result && target);
        QTRY_VERIFY_WITH_TIMEOUT(tree->topLevelItemCount() == 3 && tree->topLevelItem(0)->childCount() == 2, 2000);
        auto* a = tree->topLevelItem(0)->child(0);
        auto* b = tree->topLevelItem(0)->child(1);
        tree->setCurrentItem(a);
        info->click();
        QTRY_COMPARE_WITH_TIMEOUT(api.queries.size(), 1, 2000);
        const auto old_reply = pending_device_query(window);
        QVERIFY(old_reply);
        // 旧回复已到TCP但尚未分发，切换目标后才运行事件循环。
        respond(api.queries[0].socket,
                old_http_status,
                old_http_status == 200
                    ? device_query_response(QStringLiteral("34020000001320000001"), QStringLiteral("DeviceInfo"), QStringLiteral("stale-old-device"))
                    : QByteArray(R"({"error":"stale-old-device"})"));
        tree->setCurrentItem(b);
        if (return_to_a)
        {
            tree->setCurrentItem(a);
        }
        const auto expected = return_to_a ? QStringLiteral("34020000001320000001") : QStringLiteral("34020000001320000002");
        QVERIFY(result->toPlainText().isEmpty());
        QVERIFY(target->text().contains(expected));
        status->click();
        QTRY_COMPARE_WITH_TIMEOUT(api.queries.size(), 2, 2000);
        QTRY_VERIFY_WITH_TIMEOUT(old_reply.isNull(), 1000);
        QVERIFY(!info->isEnabled() && !status->isEnabled());
        QVERIFY(!result->toPlainText().contains(QStringLiteral("stale-old-device")));
        QCOMPARE(api.queries[1].path, QByteArray("/api/devices/") + expected.toLatin1() + "/query");
        QCOMPARE(api.queries[1].body, QJsonObject({{"command", "DeviceStatus"}}));
        respond(api.queries[1].socket, 200, device_query_response(expected, QStringLiteral("DeviceStatus"), QStringLiteral("fresh-current-device")));
        QTRY_VERIFY_WITH_TIMEOUT(info->isEnabled() && status->isEnabled(), 1000);
        QVERIFY(result->toPlainText().contains(QStringLiteral("fresh-current-device")));
        QVERIFY(!result->toPlainText().contains(QStringLiteral("stale-old-device")));
        QVERIFY(target->text().contains(expected));
        window.close();
    }

    void device_query_reset_cancels_callback_data()
    {
        QTest::addColumn<QString>("action");
        QTest::newRow("refresh-tree") << QStringLiteral("refresh");
        QTest::newRow("change-platform") << QStringLiteral("platform");
        QTest::newRow("close-window") << QStringLiteral("close");
    }

    void device_query_reset_cancels_callback()
    {
        QFETCH(QString, action);
        platform_fixture api(true);
        QVERIFY(api.start());
        monitor_window window;
        window.show();
        auto* tree = window.findChild<QTreeWidget*>(QStringLiteral("device_tree"));
        auto* info = window.findChild<QPushButton*>(QStringLiteral("device_info_query"));
        auto* status = window.findChild<QPushButton*>(QStringLiteral("device_status_query"));
        auto* result = window.findChild<QPlainTextEdit*>(QStringLiteral("device_query_result"));
        auto* target = window.findChild<QLabel*>(QStringLiteral("device_query_target"));
        auto* refresh = window.findChild<QPushButton*>(QStringLiteral("refresh_devices"));
        auto* address = window.findChild<QLineEdit*>(QStringLiteral("platform_address"));
        QVERIFY(tree && info && status && result && target && refresh && address);
        QTRY_VERIFY_WITH_TIMEOUT(tree->topLevelItemCount() == 3 && tree->topLevelItem(0)->childCount() == 2, 2000);
        tree->setCurrentItem(tree->topLevelItem(0)->child(0));
        info->click();
        QTRY_COMPARE_WITH_TIMEOUT(api.queries.size(), 1, 2000);
        const auto old_reply = pending_device_query(window);
        QVERIFY(old_reply);
        respond(api.queries[0].socket,
                200,
                device_query_response(QStringLiteral("34020000001320000001"), QStringLiteral("DeviceInfo"), QStringLiteral("late-after-reset")));
        platform_fixture second(true);
        if (action == QStringLiteral("close"))
        {
            window.close();
        }
        else
        {
            if (action == QStringLiteral("platform"))
            {
                QVERIFY(second.start());
                address->setText(QStringLiteral("http://127.0.0.1:%1").arg(second.server.serverPort()));
            }
            refresh->click();
        }
        const auto after_reset = result->toPlainText();
        const auto after_target = target->text();
        QTRY_VERIFY_WITH_TIMEOUT(old_reply.isNull(), 1000);
        QCOMPARE(result->toPlainText(), after_reset);
        QCOMPARE(target->text(), after_target);
        QVERIFY(!result->toPlainText().contains(QStringLiteral("late-after-reset")));
        if (action != QStringLiteral("close"))
        {
            QVERIFY(result->toPlainText().isEmpty());
            QVERIFY(!info->isEnabled() && !status->isEnabled());
            QTRY_VERIFY_WITH_TIMEOUT(tree->topLevelItemCount() == 3 && tree->topLevelItem(0)->childCount() == 2, 2000);
            tree->setCurrentItem(tree->topLevelItem(0)->child(1));
            status->click();
            auto& active = action == QStringLiteral("platform") ? second : api;
            const int count = action == QStringLiteral("platform") ? 1 : 2;
            QTRY_COMPARE_WITH_TIMEOUT(active.queries.size(), count, 2000);
            respond(active.queries.back().socket,
                    200,
                    device_query_response(QStringLiteral("34020000001320000002"), QStringLiteral("DeviceStatus"), QStringLiteral("new-after-reset")));
            QTRY_VERIFY_WITH_TIMEOUT(result->toPlainText().contains(QStringLiteral("new-after-reset")), 1000);
            QVERIFY(info->isEnabled() && status->isEnabled());
            window.close();
        }
        else
        {
            QVERIFY(!window.isVisible());
        }
    }

    void device_query_direct_destruction()
    {
        platform_fixture api(true);
        QVERIFY(api.start());
        auto window = std::make_unique<monitor_window>();
        window->show();
        auto* tree = window->findChild<QTreeWidget*>(QStringLiteral("device_tree"));
        auto* info = window->findChild<QPushButton*>(QStringLiteral("device_info_query"));
        QVERIFY(tree && info);
        QTRY_VERIFY_WITH_TIMEOUT(tree->topLevelItemCount() == 3 && tree->topLevelItem(0)->childCount() == 2, 2000);
        tree->setCurrentItem(tree->topLevelItem(0)->child(0));
        info->click();
        QTRY_COMPARE_WITH_TIMEOUT(api.queries.size(), 1, 2000);
        const auto reply = pending_device_query(*window);
        const auto peer = api.queries[0].socket;
        QPointer<monitor_window> guarded = window.get();
        QVERIFY(reply && peer);
        QCOMPARE(peer->state(), QAbstractSocket::ConnectedState);
        // 不经过closeEvent：选中树项和pending查询随窗口析构，迟到HTTP仍由真实TCP投递。
        window.reset();
        QVERIFY(guarded.isNull());
        QVERIFY(reply.isNull());
        if (peer && peer->state() == QAbstractSocket::ConnectedState)
        {
            respond(peer,
                    200,
                    device_query_response(
                        QStringLiteral("34020000001320000001"), QStringLiteral("DeviceInfo"), QStringLiteral("late-after-direct-destruction")));
        }
        QTRY_VERIFY_WITH_TIMEOUT(peer.isNull(), 1000);
        QVERIFY(QCoreApplication::instance()->findChildren<decode_thread*>().isEmpty());
    }

    void device_query_failures_restore_buttons_data()
    {
        QTest::addColumn<int>("http_status");
        QTest::addColumn<QByteArray>("body");
        QTest::addColumn<QString>("expected");
        QTest::newRow("http-error") << 502 << QByteArray(R"({"error":"query_backend_failed"})") << QStringLiteral("502");
        QTest::newRow("device-refused") << 200
                                        << device_query_response(
                                               QStringLiteral("34020000001320000001"), QStringLiteral("DeviceInfo"), {}, QStringLiteral("ERROR"))
                                        << QStringLiteral("设备拒绝");
        QTest::newRow("invalid-json") << 200 << QByteArray("{broken") << QStringLiteral("JSON");
        QTest::newRow("wrong-device") << 200 << device_query_response(QStringLiteral("34020000001320000002"), QStringLiteral("DeviceInfo"), {})
                                      << QStringLiteral("无效");
        QTest::newRow("wrong-command") << 200 << device_query_response(QStringLiteral("34020000001320000001"), QStringLiteral("DeviceStatus"), {})
                                       << QStringLiteral("无效");
        QTest::newRow("unknown-result") << 200
                                        << device_query_response(
                                               QStringLiteral("34020000001320000001"), QStringLiteral("DeviceInfo"), {}, QStringLiteral("unknown"))
                                        << QStringLiteral("无效");
        QTest::newRow("invalid-payload") << 200
                                         << QByteArray(
                                                R"({"device_id":"34020000001320000001","command":"DeviceInfo","sn":17,"result":"OK","info":[]})")
                                         << QStringLiteral("无效");
        QTest::newRow("network-timeout") << 0 << QByteArray{} << QStringLiteral("timed out");
    }

    void device_query_failures_restore_buttons()
    {
        QFETCH(int, http_status);
        QFETCH(QByteArray, body);
        QFETCH(QString, expected);
        platform_fixture api(true);
        QVERIFY(api.start());
        monitor_window window;
        auto* tree = window.findChild<QTreeWidget*>(QStringLiteral("device_tree"));
        auto* info = window.findChild<QPushButton*>(QStringLiteral("device_info_query"));
        auto* status = window.findChild<QPushButton*>(QStringLiteral("device_status_query"));
        auto* result = window.findChild<QPlainTextEdit*>(QStringLiteral("device_query_result"));
        QVERIFY(tree && info && status && result);
        QTRY_VERIFY_WITH_TIMEOUT(tree->topLevelItemCount() == 3 && tree->topLevelItem(0)->childCount() == 2, 2000);
        tree->setCurrentItem(tree->topLevelItem(0)->child(0));
        info->click();
        QTRY_COMPARE_WITH_TIMEOUT(api.queries.size(), 1, 2000);
        const auto reply = pending_device_query(window);
        QVERIFY(reply);
        QVERIFY(!info->isEnabled() && !status->isEnabled());
        if (http_status != 0)
        {
            respond(api.queries[0].socket, http_status, body);
        }
        QTRY_VERIFY_WITH_TIMEOUT(info->isEnabled() && status->isEnabled(), 12'000);
        QVERIFY2(result->toPlainText().contains(expected, Qt::CaseInsensitive), qPrintable(result->toPlainText()));
        QTRY_VERIFY_WITH_TIMEOUT(reply.isNull(), 1000);
        status->click();
        QTRY_COMPARE_WITH_TIMEOUT(api.queries.size(), 2, 2000);
        QCOMPARE(api.queries[1].body, QJsonObject({{"command", "DeviceStatus"}}));
        respond(api.queries[1].socket,
                200,
                device_query_response(QStringLiteral("34020000001320000001"), QStringLiteral("DeviceStatus"), QStringLiteral("manual-retry-ok")));
        QTRY_VERIFY_WITH_TIMEOUT(result->toPlainText().contains(QStringLiteral("manual-retry-ok")), 1000);
        QVERIFY(info->isEnabled() && status->isEnabled());
        window.close();
    }

    void layout_selection_and_shrink()
    {
        platform_fixture api;
        QVERIFY(api.start());
        monitor_window window;
        window.show();
        auto* layout = window.findChild<QComboBox*>(QStringLiteral("view_layout"));
        auto* tree = window.findChild<QTreeWidget*>(QStringLiteral("device_tree"));
        QVERIFY(layout && tree);
        QTRY_VERIFY_WITH_TIMEOUT(tree->topLevelItemCount() == 3 && tree->topLevelItem(2)->childCount() == 3, 2000);
        QCOMPARE(layout->currentData().toInt(), 1);
        for (const int count : {1, 4, 9, 16})
        {
            const int index = layout->findData(count);
            QVERIFY(index >= 0);
            layout->setCurrentIndex(index);
            for (int slot = 1; slot <= 16; ++slot)
            {
                auto* view = slot_view(window, slot);
                QVERIFY(view);
                QCOMPARE(!view->isHidden(), slot <= count);
            }
        }
        auto* last = slot_view(window, 16);
        QTest::mouseClick(last, Qt::LeftButton);
        tree->itemDoubleClicked(tree->topLevelItem(2)->child(0), 0);
        QTRY_COMPARE_WITH_TIMEOUT(api.plays.size(), 1, 2000);
        QCOMPARE(api.plays[0].id, QStringLiteral("a"));
        QVERIFY(last->status().contains(QStringLiteral("camera-a")));
        const auto old_peer = api.plays[0].socket;

        layout->setCurrentIndex(layout->findData(4));
        QCOMPARE(last->status(), QStringLiteral("已停止"));
        QTRY_VERIFY_WITH_TIMEOUT(!old_peer || old_peer->state() == QAbstractSocket::UnconnectedState, 1000);
        tree->itemDoubleClicked(tree->topLevelItem(2)->child(1), 0);
        QTRY_COMPARE_WITH_TIMEOUT(api.plays.size(), 2, 2000);
        QVERIFY(slot_view(window, 1)->status().contains(QStringLiteral("camera-b")));
        for (int slot = 2; slot <= 16; ++slot)
        {
            QVERIFY(!slot_view(window, slot)->status().contains(QStringLiteral("camera-b")));
        }
        layout->setCurrentIndex(layout->findData(16));
        QCOMPARE(api.plays.size(), 2);
        QCOMPARE(last->status(), QStringLiteral("已停止"));
        window.close();
    }

    void drop_replaces_only_target()
    {
        platform_fixture api;
        QVERIFY(api.start());
        monitor_window window;
        window.show();
        auto* layout = window.findChild<QComboBox*>(QStringLiteral("view_layout"));
        auto* tree = window.findChild<QTreeWidget*>(QStringLiteral("device_tree"));
        QVERIFY(layout && tree);
        layout->setCurrentIndex(layout->findData(4));
        QTRY_VERIFY_WITH_TIMEOUT(tree->topLevelItemCount() == 3 && tree->topLevelItem(2)->childCount() == 3, 2000);
        auto* second = slot_view(window, 2);
        auto* third = slot_view(window, 3);
        QVERIFY(second && third);
        QTest::mouseClick(second, Qt::LeftButton);
        tree->itemDoubleClicked(tree->topLevelItem(2)->child(0), 0);
        QTRY_COMPARE_WITH_TIMEOUT(api.plays.size(), 1, 2000);
        QTest::mouseClick(third, Qt::LeftButton);
        tree->itemDoubleClicked(tree->topLevelItem(2)->child(1), 0);
        QTRY_COMPARE_WITH_TIMEOUT(api.plays.size(), 2, 2000);
        const auto old_peer = api.plays[0].socket;
        QVERIFY(old_peer);
        const auto third_status = third->status();
        // 旧错误已到 TCP，尚未分发；drop 替换只能取消第二格，不能串改第三格。
        respond(old_peer, 500, R"({"error":"late-old-error"})");
        QVERIFY(drop_item(second, item_payload({{"kind", "push_device"}, {"id", "c"}, {"name", "camera-c"}})));
        QTRY_COMPARE_WITH_TIMEOUT(api.plays.size(), 3, 2000);
        QCOMPARE(api.plays[2].id, QStringLiteral("c"));
        QTRY_VERIFY_WITH_TIMEOUT(!old_peer || old_peer->state() == QAbstractSocket::UnconnectedState, 1000);
        QVERIFY(second->status().contains(QStringLiteral("camera-c")));
        QVERIFY(!second->status().contains(QStringLiteral("late-old-error")));
        QCOMPARE(third->status(), third_status);
        QVERIFY(api.plays[1].socket && api.plays[1].socket->state() == QAbstractSocket::ConnectedState);
        window.close();
    }

    void cancel_all_views_data()
    {
        QTest::addColumn<bool>("close_window");
        QTest::newRow("stop-all") << false;
        QTest::newRow("close-window") << true;
    }

    void cancel_all_views()
    {
        QFETCH(bool, close_window);
        platform_fixture api;
        QTcpServer rtsp;
        QVERIFY(api.start());
        QVERIFY(rtsp.listen(QHostAddress::LocalHost, 0));
        monitor_window window;
        const auto cleanup = qScopeGuard(
            [&]()
            {
                for (auto* view : window.findChildren<playback_view*>())
                {
                    view->stop();
                }
                for (auto* thread : QCoreApplication::instance()->findChildren<decode_thread*>())
                {
                    thread->requestInterruption();
                    if (!thread->wait(3000))
                    {
                        qFatal("多画面关闭后解码线程未退出");
                    }
                }
            });
        window.show();
        auto* layout = window.findChild<QComboBox*>(QStringLiteral("view_layout"));
        auto* tree = window.findChild<QTreeWidget*>(QStringLiteral("device_tree"));
        auto* stop_all = window.findChild<QPushButton*>(QStringLiteral("stop_all"));
        QVERIFY(layout && tree && stop_all);
        layout->setCurrentIndex(layout->findData(4));
        QTRY_VERIFY_WITH_TIMEOUT(tree->topLevelItemCount() == 3 && tree->topLevelItem(2)->childCount() == 3, 2000);
        for (int slot = 1; slot <= 3; ++slot)
        {
            auto* view = slot_view(window, slot);
            QVERIFY(view);
            QTest::mouseClick(view, Qt::LeftButton);
            tree->itemDoubleClicked(tree->topLevelItem(2)->child(slot - 1), 0);
            QTRY_COMPARE_WITH_TIMEOUT(api.plays.size(), slot, 2000);
        }
        QTRY_COMPARE_WITH_TIMEOUT(api.plays.size(), 3, 2000);
        const auto url = QStringLiteral("rtsp://127.0.0.1:%1/hang").arg(rtsp.serverPort());
        respond(api.plays[0].socket, 200, QJsonDocument(QJsonObject{{"rtsp_url", url}}).toJson(QJsonDocument::Compact));
        QTRY_VERIFY_WITH_TIMEOUT(rtsp.hasPendingConnections(), 2000);
        auto* peer = rtsp.nextPendingConnection();
        QVERIFY(peer);
        QTRY_VERIFY_WITH_TIMEOUT(peer->peek(65536).contains("\r\n\r\n"), 1000);
        QVERIFY(peer->peek(65536).startsWith("OPTIONS rtsp://"));
        const auto threads = QCoreApplication::instance()->findChildren<decode_thread*>();
        QCOMPARE(threads.size(), 1);
        QPointer<decode_thread> decoder = threads.front();
        QList<QPointer<QNetworkReply>> replies;
        for (auto* reply : window.findChildren<QNetworkReply*>())
        {
            if (!reply->isFinished())
            {
                replies.append(reply);
            }
        }
        QCOMPARE(replies.size(), 2);
        // 第二格迟到成功不能在全停或关闭后新建 RTSP decoder。
        respond(api.plays[1].socket, 200, QJsonDocument(QJsonObject{{"rtsp_url", url}}).toJson(QJsonDocument::Compact));
        if (close_window)
        {
            window.close();
        }
        else
        {
            stop_all->click();
            stop_all->click();
        }
        for (int slot = 1; slot <= 16; ++slot)
        {
            auto* view = slot_view(window, slot);
            QVERIFY(view);
            QCOMPARE(view->status(), QStringLiteral("已停止"));
        }
        QVERIFY(!decoder || decoder->isInterruptionRequested());
        QTRY_VERIFY_WITH_TIMEOUT(decoder.isNull(), 3000);
        QTRY_COMPARE_WITH_TIMEOUT(peer->state(), QAbstractSocket::UnconnectedState, 1000);
        for (const auto& reply : replies)
        {
            QTRY_VERIFY_WITH_TIMEOUT(reply.isNull(), 1000);
        }
        QVERIFY(!rtsp.hasPendingConnections());
        QVERIFY(QCoreApplication::instance()->findChildren<decode_thread*>().isEmpty());
        QCOMPARE(api.plays.size(), 3);
        if (close_window)
        {
            QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 1000);
        }
        else
        {
            QVERIFY(window.isVisible());
            window.close();
        }
    }

    void preferences_restore_without_playback()
    {
        platform_fixture api;
        QVERIFY(api.start());
        {
            monitor_window window;
            auto* layout = window.findChild<QComboBox*>(QStringLiteral("view_layout"));
            auto* aspect = window.findChild<QCheckBox*>(QStringLiteral("keep_aspect"));
            auto* sound = window.findChild<QPushButton*>(QStringLiteral("sound_toggle"));
            QVERIFY(layout && aspect && sound);
            layout->setCurrentIndex(layout->findData(9));
            aspect->setChecked(false);
            sound->click();
            window.resize(1100, 740);
            window.close();
        }
        monitor_window restored;
        restored.show();
        auto* layout = restored.findChild<QComboBox*>(QStringLiteral("view_layout"));
        auto* aspect = restored.findChild<QCheckBox*>(QStringLiteral("keep_aspect"));
        auto* sound = restored.findChild<QPushButton*>(QStringLiteral("sound_toggle"));
        auto* tree = restored.findChild<QTreeWidget*>(QStringLiteral("device_tree"));
        QVERIFY(layout && aspect && sound && tree);
        QTRY_VERIFY_WITH_TIMEOUT(tree->topLevelItemCount() == 3 && tree->topLevelItem(2)->childCount() == 3, 2000);
        QCOMPARE(layout->currentData().toInt(), 9);
        QVERIFY(!aspect->isChecked());
        QVERIFY(!sound->isChecked());
        QCOMPARE(api.plays.size(), 0);
        QVERIFY(QCoreApplication::instance()->findChildren<decode_thread*>().isEmpty());
        for (int slot = 1; slot <= 16; ++slot)
        {
            auto* view = slot_view(restored, slot);
            QVERIFY(view);
            QCOMPARE(!view->isHidden(), slot <= 9);
            QVERIFY(!view->audible());
            QCOMPARE(view->snapshot().video_frames, quint64{0});
        }
        restored.close();
    }

    void invalid_drop_data()
    {
        QTest::addColumn<QByteArray>("payload");
        QTest::addColumn<bool>("model_format");
        QTest::newRow("other-mime") << QByteArray("camera-c") << false;
        QTest::newRow("truncated-model-data") << QByteArray("broken") << true;
        QTest::newRow("root-item") << item_payload({}) << true;
        QTest::newRow("unknown-kind") << item_payload({{"kind", "unknown"}, {"id", "c"}}) << true;
        QTest::newRow("missing-id") << item_payload({{"kind", "push_device"}, {"name", "camera-c"}}) << true;
        QTest::newRow("gb-device-not-channel") << item_payload({{"kind", "gb_device"}, {"id", "c"}}) << true;
    }

    void invalid_drop()
    {
        QFETCH(QByteArray, payload);
        QFETCH(bool, model_format);
        platform_fixture api;
        QVERIFY(api.start());
        monitor_window window;
        window.show();
        auto* tree = window.findChild<QTreeWidget*>(QStringLiteral("device_tree"));
        auto* view = slot_view(window, 1);
        QVERIFY(tree && view);
        QTRY_VERIFY_WITH_TIMEOUT(tree->topLevelItemCount() == 3 && tree->topLevelItem(2)->childCount() == 3, 2000);
        tree->itemDoubleClicked(tree->topLevelItem(2)->child(0), 0);
        QTRY_COMPARE_WITH_TIMEOUT(api.plays.size(), 1, 2000);
        const auto before = view->status();
        QVERIFY(!drop_item(view, payload, model_format));
        QCOMPARE(view->status(), before);
        QCOMPARE(api.plays.size(), 1);
        QVERIFY(api.plays[0].socket && api.plays[0].socket->state() == QAbstractSocket::ConnectedState);
        window.close();
    }

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

   private:
    std::unique_ptr<QTemporaryDir> settings_;
};

QTEST_MAIN(monitor_window_test)
#include "monitor_window_test.moc"
