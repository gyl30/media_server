#include <memory>
#include <utility>

#include <QMap>
#include <QTest>
#include <QList>
#include <QLabel>
#include <QTimer>
#include <QVariant>
#include <QPointer>
#include <QSpinBox>
#include <QLineEdit>
#include <QComboBox>
#include <QCheckBox>
#include <QMimeData>
#include <QSettings>
#include <QIODevice>
#include <QByteArray>
#include <QJsonArray>
#include <QJsonValue>
#include <QDropEvent>
#include <QEventLoop>
#include <QTcpServer>
#include <QTcpSocket>
#include <QDataStream>
#include <QStringList>
#include <QScopeGuard>
#include <QJsonObject>
#include <QPushButton>
#include <QTreeWidget>
#include <QMetaObject>
#include <QMessageBox>
#include <QHostAddress>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QTemporaryDir>
#include <QPlainTextEdit>
#include <QAbstractButton>
#include <QAbstractSocket>
#include <QDragEnterEvent>
#include <QCoreApplication>

#include "desktop/playback_view.h"
#include "desktop/monitor_window.h"
#include "desktop/record_query_dialog.h"

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

    explicit platform_fixture(bool gb_devices = false, bool hold_gb_play = false)
    {
        connect(
            &server,
            &QTcpServer::newConnection,
            this,
            [this, gb_devices, hold_gb_play]()
            {
                while (server.hasPendingConnections())
                {
                    auto* socket = server.nextPendingConnection();
                    connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                    connect(
                        socket,
                        &QTcpSocket::readyRead,
                        this,
                        [this, socket, gb_devices, hold_gb_play]()
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
                            if (path.endsWith("/query") || path.endsWith("/ptz"))
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
                            if (path.endsWith("/playback") || (hold_gb_play && path.endsWith("/play")))
                            {
                                auto& pending = path.endsWith("/playback") ? record_plays : gb_plays;
                                pending.append(
                                    {socket, bytes.left(bytes.indexOf("\r\n")), path, QJsonDocument::fromJson(bytes.mid(end + 4, length)).object()});
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
    QList<pending_query> record_plays;
    QList<pending_query> gb_plays;
};

QByteArray device_query_response(const QString& id, const QString& command, const QString& marker, const QString& result = QStringLiteral("OK"))
{
    QJsonObject response{{"device_id", id}, {"command", command}, {"sn", 17}, {"result", result}};
    response.insert(command == QStringLiteral("DeviceInfo") ? QStringLiteral("info") : QStringLiteral("status"),
                    command == QStringLiteral("DeviceInfo") ? QJsonObject{{"device_name", marker}, {"manufacturer", "fixture-manufacturer"}}
                                                            : QJsonObject{{"online", "ONLINE"}, {"status", "OK"}, {"reason", marker}});
    return QJsonDocument(response).toJson(QJsonDocument::Compact);
}

QJsonObject preset_query_response(const QString& device, const QString& channel, const QJsonArray& presets)
{
    return {{"device_id", device}, {"channel_id", channel}, {"command", "PresetQuery"}, {"sn", 23}, {"presets", presets}};
}

const QStringList ptz_commands{QStringLiteral("left"),
                               QStringLiteral("right"),
                               QStringLiteral("up"),
                               QStringLiteral("down"),
                               QStringLiteral("up_left"),
                               QStringLiteral("up_right"),
                               QStringLiteral("down_left"),
                               QStringLiteral("down_right"),
                               QStringLiteral("zoom_in"),
                               QStringLiteral("zoom_out"),
                               QStringLiteral("stop")};

QJsonObject ptz_response(const QString& command, const QString& device = QStringLiteral("34020000001320000001"))
{
    QJsonObject response{{"device_id", device},
                         {"channel_id", "34020000001310000001"},
                         {"command", command},
                         {"stop", "accepted"},
                         {"physical_result", "unconfirmed"}};
    if (command != QStringLiteral("stop"))
    {
        response.insert(QStringLiteral("start"), QStringLiteral("accepted"));
    }
    return response;
}

QJsonObject preset_control_response(const QString& command, int number)
{
    return {{"device_id", "34020000001320000001"},
            {"channel_id", "34020000001310000001"},
            {"command", command},
            {"preset_id", number},
            {"accepted", true},
            {"physical_result", "unconfirmed"}};
}

bool select_gb_channel(monitor_window& window, int index = 0)
{
    auto* tree = window.findChild<QTreeWidget*>(QStringLiteral("device_tree"));
    if (!tree || !QTest::qWaitFor([&] { return tree->topLevelItemCount() == 3 && tree->topLevelItem(0)->childCount() == 2; }, 2000))
    {
        return false;
    }
    auto* device = tree->topLevelItem(0)->child(index);
    if (device->childCount() == 0)
    {
        tree->itemDoubleClicked(device, 0);
    }
    if (!QTest::qWaitFor([&] { return device->childCount() == 1; }, 2000))
    {
        return false;
    }
    tree->setCurrentItem(device->child(0));
    return true;
}

bool load_preset_rows(monitor_window& window, platform_fixture& api, const QJsonArray& rows)
{
    auto* button = window.findChild<QPushButton*>(QStringLiteral("preset_query"));
    auto* list = window.findChild<QTreeWidget*>(QStringLiteral("preset_list"));
    auto* tree = window.findChild<QTreeWidget*>(QStringLiteral("device_tree"));
    if (!button || !list || !tree || !tree->currentItem() || !button->isEnabled())
    {
        return false;
    }
    const auto data = tree->currentItem()->data(0, Qt::UserRole).toMap();
    const int count = api.queries.size();
    button->click();
    if (!QTest::qWaitFor([&] { return api.queries.size() == count + 1; }, 2000))
    {
        return false;
    }
    respond(
        api.queries.back().socket,
        200,
        QJsonDocument(preset_query_response(data.value(QStringLiteral("id")).toString(), data.value(QStringLiteral("channel_id")).toString(), rows))
            .toJson());
    return QTest::qWaitFor([&] { return button->isEnabled() && list->topLevelItemCount() == rows.size(); }, 2000);
}

QPointer<QNetworkReply> pending_ptz(monitor_window& window)
{
    for (auto* reply : window.findChildren<QNetworkReply*>())
    {
        if (!reply->isFinished() && reply->url().path().endsWith(QStringLiteral("/ptz")))
        {
            return reply;
        }
    }
    return {};
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

bool load_record_rows(record_query_dialog& dialog, platform_fixture& api)
{
    auto* query = dialog.findChild<QPushButton*>(QStringLiteral("record_query"));
    auto* list = dialog.findChild<QTreeWidget*>(QStringLiteral("record_list"));
    auto* offset = dialog.findChild<QSpinBox*>(QStringLiteral("record_utc_offset"));
    if (!query || !list || !offset)
    {
        return false;
    }
    offset->setValue(0);
    const auto count = api.queries.size();
    query->click();
    if (!QTest::qWaitFor([&] { return api.queries.size() == count + 1; }, 1000))
    {
        return false;
    }
    const QJsonArray rows{
        QJsonObject{
            {"device_id", "34020000001310000001"}, {"name", "first"}, {"start_time", "2023-11-14T22:13:20"}, {"end_time", "2023-11-14T22:13:25"}},
        QJsonObject{
            {"device_id", "34020000001310000001"}, {"name", "selected"}, {"start_time", "2023-11-14T22:13:30"}, {"end_time", "2023-11-14T22:13:40"}}};
    respond(api.queries.back().socket,
            200,
            QJsonDocument(QJsonObject{{"device_id", "34020000001320000001"},
                                      {"channel_id", "34020000001310000001"},
                                      {"command", "RecordInfo"},
                                      {"sn", 29},
                                      {"sum_num", 2},
                                      {"records", rows}})
                .toJson(QJsonDocument::Compact));
    if (!QTest::qWaitFor([&] { return query->isEnabled() && list->topLevelItemCount() == 2; }, 1000))
    {
        return false;
    }
    list->setCurrentItem(list->topLevelItem(1));
    return list->topLevelItem(1)->isSelected();
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
    void record_playback_queued_reload_data()
    {
        QTest::addColumn<QString>("action");
        for (const auto& action : {"refresh", "platform", "dialog-close", "dialog-reopen"})
        {
            QTest::newRow(action) << QString::fromLatin1(action);
        }
    }

    void record_playback_queued_reload()
    {
        QFETCH(QString, action);
        platform_fixture api(true);
        QVERIFY(api.start());
        monitor_window window;
        QVERIFY(select_gb_channel(window));
        window.findChild<QPushButton*>(QStringLiteral("open_record_query"))->click();
        QPointer<record_query_dialog> dialog = window.findChild<record_query_dialog*>();
        QVERIFY(dialog);
        QVERIFY(load_record_rows(*dialog, api));
        auto* play = dialog->findChild<QPushButton*>(QStringLiteral("record_play"));
        QVERIFY(play && play->isEnabled());
        platform_fixture second(true);
        if (action == QStringLiteral("platform"))
        {
            QVERIFY(second.start());
            window.findChild<QLineEdit*>(QStringLiteral("platform_address"))
                ->setText(QStringLiteral("http://127.0.0.1:%1").arg(second.server.serverPort()));
        }
        // 点击已排队但尚未派发；同步操作先关闭旧查询窗口。
        QVERIFY(QMetaObject::invokeMethod(play, &QPushButton::click, Qt::QueuedConnection));
        if (action == QStringLiteral("dialog-close"))
        {
            dialog->close();
        }
        else if (action == QStringLiteral("dialog-reopen"))
        {
            window.findChild<QPushButton*>(QStringLiteral("open_record_query"))->click();
            bool fresh_visible = false;
            for (auto* candidate : window.findChildren<record_query_dialog*>())
            {
                fresh_visible = fresh_visible || (candidate != dialog && candidate->isVisible());
            }
            QVERIFY(fresh_visible);
        }
        else
        {
            window.findChild<QPushButton*>(QStringLiteral("refresh_devices"))->click();
        }
        QVERIFY(!dialog || !dialog->isVisible());
        QVERIFY(QTest::qWaitFor([&] { return dialog.isNull(); }, 1000));
        QVERIFY(select_gb_channel(window));
        // 给真实网络请求一个有限观察窗口；不是平台时延或设备协议承诺。
        QEventLoop observation;
        QTimer::singleShot(250, &observation, &QEventLoop::quit);
        observation.exec();
        QCOMPARE(api.record_plays.size(), 0);
        QCOMPARE(second.record_plays.size(), 0);
        window.close();
    }

    void record_playback_current_view_and_fixed_target_data()
    {
        QTest::addColumn<int>("selected_slot");
        QTest::newRow("original-view") << 1;
        QTest::newRow("newly-selected-view") << 2;
    }

    void record_playback_current_view_and_fixed_target()
    {
        QFETCH(int, selected_slot);
        platform_fixture api(true, true);
        QVERIFY(api.start());
        monitor_window window;
        window.show();
        auto* layout = window.findChild<QComboBox*>(QStringLiteral("view_layout"));
        QVERIFY(layout);
        layout->setCurrentIndex(layout->findData(4));
        QVERIFY(select_gb_channel(window));
        auto* tree = window.findChild<QTreeWidget*>(QStringLiteral("device_tree"));
        auto* first = slot_view(window, 1);
        auto* second = slot_view(window, 2);
        auto* live = slot_view(window, 3);
        QVERIFY(tree && first && second && live && live->isVisible());
        QTest::mouseClick(live, Qt::LeftButton);
        tree->itemDoubleClicked(tree->currentItem(), 0);
        QVERIFY(QTest::qWaitFor([&] { return api.gb_plays.size() == 1; }, 1000));
        const auto live_peer = api.gb_plays[0].socket;
        const auto live_status = live->status();
        // 回放控制只作用于选中画面的有限回放，实时画面不可暂停。
        auto* pause = window.findChild<QPushButton*>(QStringLiteral("record_pause"));
        QVERIFY(pause && !pause->isEnabled());
        // 只保留独立实时观看申请；不把挂起的 HTTP 请求冒称 SIP 上游已建立。
        QTest::mouseClick(first, Qt::LeftButton);
        window.findChild<QPushButton*>(QStringLiteral("open_record_query"))->click();
        auto* dialog = window.findChild<record_query_dialog*>();
        QVERIFY(dialog);
        QVERIFY(load_record_rows(*dialog, api));
        QCOMPARE(api.queries.back().path, QByteArray("/api/devices/34020000001320000001/channels/34020000001310000001/records/query"));
        QVERIFY(select_gb_channel(window, 1));
        auto* target = slot_view(window, selected_slot);
        auto* other = slot_view(window, selected_slot == 1 ? 2 : 1);
        const auto other_status = other->status();
        QTest::mouseClick(target, Qt::LeftButton);
        QVERIFY(target->property("selected").toBool());
        dialog->findChild<QPushButton*>(QStringLiteral("record_play"))->click();
        QVERIFY(QTest::qWaitFor([&] { return api.record_plays.size() == 1; }, 1000));
        const auto& request = api.record_plays[0];
        QCOMPARE(request.request_line, QByteArray("POST /api/devices/34020000001320000001/channels/34020000001310000001/playback HTTP/1.1"));
        QCOMPARE(request.body, (QJsonObject{{"start_time", 1700000010}, {"end_time", 1700000020}}));
        QVERIFY(!pause->isEnabled());
        QCOMPARE(other->status(), other_status);
        QVERIFY(live_peer && live_peer->state() == QAbstractSocket::ConnectedState);
        QCOMPARE(live->status(), live_status);
        QCOMPARE(api.gb_plays.size(), 1);
        respond(request.socket, 502, R"({"error":"fixture_rejected"})");
        QVERIFY(QTest::qWaitFor([&] { return target->status().contains(QStringLiteral("录像回放申请失败")); }, 1000));
        QVERIFY(!pause->isEnabled());
        QCOMPARE(other->status(), other_status);
        QVERIFY(live_peer && live_peer->state() == QAbstractSocket::ConnectedState);
        QCOMPARE(live->status(), live_status);
        window.close();
    }

    void record_query_entry_and_fixed_target()
    {
        platform_fixture api(true);
        QVERIFY(api.start());
        monitor_window window;
        auto* tree = window.findChild<QTreeWidget*>(QStringLiteral("device_tree"));
        auto* open = window.findChild<QPushButton*>(QStringLiteral("open_record_query"));
        QVERIFY(tree && open);
        QVERIFY(!open->isEnabled());
        QVERIFY(QTest::qWaitFor(
            [&] { return tree->topLevelItemCount() == 3 && tree->topLevelItem(0)->childCount() == 2 && tree->topLevelItem(2)->childCount() == 3; },
            2000));
        for (auto* item : {tree->topLevelItem(0), tree->topLevelItem(0)->child(0), tree->topLevelItem(2)->child(0)})
        {
            tree->setCurrentItem(item);
            QVERIFY(!open->isEnabled());
            open->click();
            QVERIFY(!window.findChild<record_query_dialog*>());
        }
        QVERIFY(select_gb_channel(window));
        QVERIFY(open->isEnabled());
        open->click();
        QPointer<record_query_dialog> first = window.findChild<record_query_dialog*>();
        QVERIFY(first && first->isVisible());
        QCOMPARE(first->windowModality(), Qt::NonModal);
        auto* first_query = first->findChild<QPushButton*>(QStringLiteral("record_query"));
        QVERIFY(first_query);
        QVERIFY(select_gb_channel(window, 1));
        QVERIFY(first->isVisible());
        first_query->click();
        QVERIFY(QTest::qWaitFor([&] { return api.queries.size() == 1; }, 1000));
        QCOMPARE(api.queries[0].path, QByteArray("/api/devices/34020000001320000001/channels/34020000001310000001/records/query"));
        const auto old_peer = api.queries[0].socket;
        open->click();
        QVERIFY(QTest::qWaitFor([&] { return first.isNull(); }, 1000));
        auto* second = window.findChild<record_query_dialog*>();
        QVERIFY(second && second->isVisible());
        second->findChild<QPushButton*>(QStringLiteral("record_query"))->click();
        QVERIFY(QTest::qWaitFor([&] { return api.queries.size() == 2; }, 1000));
        QCOMPARE(api.queries[1].path, QByteArray("/api/devices/34020000001320000002/channels/34020000001310000001/records/query"));
        QVERIFY(QTest::qWaitFor([&] { return old_peer.isNull(); }, 1000));
        window.close();
    }

    void record_query_listing_lifetime_data()
    {
        QTest::addColumn<QString>("action");
        for (const auto& action : {"refresh", "platform", "close", "destruct"})
        {
            QTest::newRow(action) << QString::fromLatin1(action);
        }
    }

    void record_query_listing_lifetime()
    {
        QFETCH(QString, action);
        platform_fixture api(true);
        QVERIFY(api.start());
        auto window = std::make_unique<monitor_window>();
        QVERIFY(select_gb_channel(*window));
        window->findChild<QPushButton*>(QStringLiteral("open_record_query"))->click();
        QPointer<record_query_dialog> dialog = window->findChild<record_query_dialog*>();
        QVERIFY(dialog);
        dialog->findChild<QPushButton*>(QStringLiteral("record_query"))->click();
        QVERIFY(QTest::qWaitFor([&] { return api.queries.size() == 1; }, 1000));
        QPointer<QNetworkReply> reply;
        for (auto* candidate : window->findChildren<QNetworkReply*>())
        {
            if (candidate->url().path().endsWith(QStringLiteral("/records/query")))
            {
                reply = candidate;
            }
        }
        const auto peer = api.queries[0].socket;
        QVERIFY(reply && peer);
        platform_fixture second(true);
        if (action == QStringLiteral("destruct"))
        {
            window.reset();
        }
        else if (action == QStringLiteral("close"))
        {
            window->close();
        }
        else
        {
            if (action == QStringLiteral("platform"))
            {
                QVERIFY(second.start());
                window->findChild<QLineEdit*>(QStringLiteral("platform_address"))
                    ->setText(QStringLiteral("http://127.0.0.1:%1").arg(second.server.serverPort()));
            }
            window->findChild<QPushButton*>(QStringLiteral("refresh_devices"))->click();
        }
        QVERIFY(QTest::qWaitFor([&] { return dialog.isNull() && reply.isNull() && peer.isNull(); }, 1000));
        if (window && action != QStringLiteral("close"))
        {
            QVERIFY(select_gb_channel(*window));
            auto& active = action == QStringLiteral("platform") ? second : api;
            window->findChild<QPushButton*>(QStringLiteral("open_record_query"))->click();
            auto* fresh = window->findChild<record_query_dialog*>();
            QVERIFY(fresh);
            fresh->findChild<QPushButton*>(QStringLiteral("record_query"))->click();
            const auto expected = action == QStringLiteral("platform") ? 1 : 2;
            QVERIFY(QTest::qWaitFor([&] { return active.queries.size() == expected; }, 1000));
            window->close();
        }
    }

    void record_query_keeps_live_and_ptz_independent()
    {
        platform_fixture api(true);
        QVERIFY(api.start());
        monitor_window window;
        auto* tree = window.findChild<QTreeWidget*>(QStringLiteral("device_tree"));
        QVERIFY(select_gb_channel(window));
        auto* left = window.findChild<QPushButton*>(QStringLiteral("ptz_left"));
        auto* stop = window.findChild<QPushButton*>(QStringLiteral("ptz_stop"));
        auto* open = window.findChild<QPushButton*>(QStringLiteral("open_record_query"));
        QVERIFY(left && stop && open);
        left->click();
        QVERIFY(QTest::qWaitFor([&] { return api.queries.size() == 1; }, 1000));
        const auto ptz = pending_ptz(window);
        QVERIFY(ptz && open->isEnabled());
        open->click();
        auto* dialog = window.findChild<record_query_dialog*>();
        QVERIFY(dialog);
        auto* query = dialog->findChild<QPushButton*>(QStringLiteral("record_query"));
        auto* cancel = dialog->findChild<QPushButton*>(QStringLiteral("record_cancel"));
        QVERIFY(query && cancel);
        query->click();
        QVERIFY(QTest::qWaitFor([&] { return api.queries.size() == 2; }, 1000));
        QVERIFY(!ptz->isFinished() && stop->isEnabled());
        respond(api.queries[0].socket, 200, QJsonDocument(ptz_response(QStringLiteral("left"))).toJson());
        QVERIFY(QTest::qWaitFor([&] { return left->isEnabled(); }, 1000));
        QVERIFY(!query->isEnabled() && cancel->isEnabled());
        // 真实观看请求在查询期间仍可发出；这里只验证独立所有权，不宣称实际解码。
        tree->itemDoubleClicked(tree->topLevelItem(2)->child(0), 0);
        QVERIFY(QTest::qWaitFor([&] { return api.plays.size() == 1; }, 1000));
        const auto play_peer = api.plays[0].socket;
        const auto play_status = slot_view(window, 1)->status();
        cancel->click();
        QVERIFY(play_peer && play_peer->state() == QAbstractSocket::ConnectedState);
        QCOMPARE(slot_view(window, 1)->status(), play_status);
        QVERIFY(left->isEnabled() && stop->isEnabled());
        window.close();
    }

    void preset_control_widgets_exist()
    {
        platform_fixture api(true);
        QVERIFY(api.start());
        monitor_window window;
        auto* tree = window.findChild<QTreeWidget*>(QStringLiteral("device_tree"));
        auto* number = window.findChild<QSpinBox*>(QStringLiteral("preset_number"));
        auto* set = window.findChild<QPushButton*>(QStringLiteral("preset_set"));
        auto* go = window.findChild<QPushButton*>(QStringLiteral("preset_goto"));
        auto* remove = window.findChild<QPushButton*>(QStringLiteral("preset_remove"));
        auto* list = window.findChild<QTreeWidget*>(QStringLiteral("preset_list"));
        QVERIFY(tree && number && set && go && remove && list);
        QCOMPARE(number->minimum(), 1);
        QCOMPARE(number->maximum(), 255);
        QCOMPARE(number->value(), 1);
        QVERIFY(!set->isEnabled() && !go->isEnabled() && !remove->isEnabled());
        QVERIFY(QTest::qWaitFor(
            [&] { return tree->topLevelItemCount() == 3 && tree->topLevelItem(0)->childCount() == 2 && tree->topLevelItem(2)->childCount() == 3; },
            2000));
        for (auto* item : {tree->topLevelItem(0), tree->topLevelItem(0)->child(0), tree->topLevelItem(2)->child(0)})
        {
            tree->setCurrentItem(item);
            QVERIFY(!set->isEnabled() && !go->isEnabled() && !remove->isEnabled());
            set->click();
            go->click();
            remove->click();
        }
        QCOMPARE(api.queries.size(), 0);
        QVERIFY(select_gb_channel(window));
        QVERIFY(set->isEnabled() && !go->isEnabled() && !remove->isEnabled());
        const QStringList ids{QStringLiteral("1"),
                              QStringLiteral("255"),
                              QStringLiteral("001"),
                              QStringLiteral("+1"),
                              QStringLiteral("1000"),
                              QStringLiteral("camera-A"),
                              QStringLiteral("0"),
                              QStringLiteral("256"),
                              QStringLiteral("１")};
        QJsonArray rows;
        for (const auto& id : ids)
        {
            rows.append(QJsonObject{{"preset_id", id}, {"preset_name", "literal-name"}});
        }
        QVERIFY(load_preset_rows(window, api, rows));
        for (int index = 0; index < ids.size(); ++index)
        {
            list->setCurrentItem(list->topLevelItem(index));
            QCOMPARE(list->topLevelItem(index)->text(0), ids[index]);
            QCOMPARE(go->isEnabled(), index < 2);
            QCOMPARE(remove->isEnabled(), index < 2);
            if (index >= 2)
            {
                go->click();
                remove->click();
            }
        }
        QCOMPARE(api.queries.size(), 1);
        QVERIFY(!window.findChild<QMessageBox*>(QStringLiteral("preset_confirm")));
        window.close();
    }

    void preset_control_requests_data()
    {
        QTest::addColumn<QString>("command");
        QTest::newRow("set") << QStringLiteral("preset_set");
        QTest::newRow("goto") << QStringLiteral("preset_goto");
        QTest::newRow("remove") << QStringLiteral("preset_remove");
    }

    void preset_control_requests()
    {
        QFETCH(QString, command);
        platform_fixture api(true);
        QVERIFY(api.start());
        monitor_window window;
        auto* list = window.findChild<QTreeWidget*>(QStringLiteral("preset_list"));
        auto* number = window.findChild<QSpinBox*>(QStringLiteral("preset_number"));
        auto* result = window.findChild<QLabel*>(QStringLiteral("preset_result"));
        auto* stop = window.findChild<QPushButton*>(QStringLiteral("ptz_stop"));
        auto* set = window.findChild<QPushButton*>(QStringLiteral("preset_set"));
        QVERIFY(list && number && result && stop && set);
        QVERIFY(select_gb_channel(window));
        QVERIFY(load_preset_rows(
            window, api, {QJsonObject{{"preset_id", "1"}, {"preset_name", "first"}}, QJsonObject{{"preset_id", "255"}, {"preset_name", "last"}}}));
        list->setCurrentItem(list->topLevelItem(1));
        number->setValue(7);
        window.findChild<QPushButton*>(command)->click();
        const int expected = command == QStringLiteral("preset_set") ? 7 : 255;
        if (command != QStringLiteral("preset_goto"))
        {
            QPointer<QMessageBox> confirmation = window.findChild<QMessageBox*>(QStringLiteral("preset_confirm"));
            QVERIFY(confirmation && confirmation->isVisible());
            QCOMPARE(confirmation->windowModality(), Qt::NonModal);
            QCOMPARE(confirmation->textFormat(), Qt::PlainText);
            QCOMPARE(confirmation->defaultButton(), confirmation->button(QMessageBox::No));
            QVERIFY(confirmation->text().contains(QStringLiteral("34020000001320000001")) &&
                    confirmation->text().contains(QStringLiteral("34020000001310000001")));
            QVERIFY(confirmation->text().contains(QString::number(expected)));
            QCOMPARE(api.queries.size(), 1);
            QCOMPARE(list->topLevelItemCount(), 2);
            QVERIFY(stop->isEnabled() && !set->isEnabled() && !list->isEnabled() && !number->isEnabled());
            confirmation->button(QMessageBox::Yes)->click();
            QVERIFY(QTest::qWaitFor([&] { return confirmation.isNull(); }, 1000));
        }
        else
        {
            QVERIFY(!window.findChild<QMessageBox*>(QStringLiteral("preset_confirm")));
        }
        QVERIFY(QTest::qWaitFor([&] { return api.queries.size() == 2; }, 2000));
        QCOMPARE(api.queries[1].request_line, QByteArray("POST /api/devices/34020000001320000001/channels/34020000001310000001/ptz HTTP/1.1"));
        QCOMPARE(api.queries[1].body, QJsonObject({{"command", command}, {"preset_id", expected}}));
        QCOMPARE(list->topLevelItemCount(), command == QStringLiteral("preset_goto") ? 2 : 0);
        QVERIFY(!list->isEnabled() && stop->isEnabled());
        for (const auto& name : {QStringLiteral("device_info_query"),
                                 QStringLiteral("device_status_query"),
                                 QStringLiteral("preset_query"),
                                 QStringLiteral("ptz_left"),
                                 QStringLiteral("preset_set"),
                                 QStringLiteral("preset_goto"),
                                 QStringLiteral("preset_remove")})
        {
            QVERIFY(!window.findChild<QPushButton*>(name)->isEnabled());
        }
        // 程序化改变选择也不能借selection lambda把pending操作解禁。
        if (command == QStringLiteral("preset_goto"))
        {
            list->setCurrentItem(list->topLevelItem(0));
        }
        QVERIFY(!window.findChild<QPushButton*>(QStringLiteral("preset_goto"))->isEnabled());
        const auto reply = pending_ptz(window);
        QVERIFY(reply);
        respond(api.queries[1].socket, 200, QJsonDocument(preset_control_response(command, expected)).toJson());
        QVERIFY(QTest::qWaitFor([&] { return set->isEnabled(); }, 2000));
        QVERIFY(result->text().contains(QStringLiteral("受理")) && result->text().contains(QStringLiteral("未确认")));
        if (command != QStringLiteral("preset_goto"))
        {
            QVERIFY(result->text().contains(QStringLiteral("查询")));
        }
        QVERIFY(QTest::qWaitFor([&] { return reply.isNull(); }, 1000));
        QCOMPARE(api.queries.size(), 2);
        window.close();
    }

    void preset_confirmation_no_preserves_list_data()
    {
        QTest::addColumn<QString>("command");
        QTest::newRow("set") << QStringLiteral("preset_set");
        QTest::newRow("remove") << QStringLiteral("preset_remove");
    }

    void preset_confirmation_no_preserves_list()
    {
        QFETCH(QString, command);
        platform_fixture api(true);
        QVERIFY(api.start());
        monitor_window window;
        QVERIFY(select_gb_channel(window));
        QVERIFY(load_preset_rows(window, api, {QJsonObject{{"preset_id", "1"}, {"preset_name", "keep-me"}}}));
        auto* list = window.findChild<QTreeWidget*>(QStringLiteral("preset_list"));
        list->setCurrentItem(list->topLevelItem(0));
        window.findChild<QPushButton*>(command)->click();
        QPointer<QMessageBox> confirmation = window.findChild<QMessageBox*>(QStringLiteral("preset_confirm"));
        QVERIFY(confirmation && confirmation->isVisible());
        confirmation->button(QMessageBox::No)->click();
        QVERIFY(QTest::qWaitFor([&] { return confirmation.isNull(); }, 1000));
        QCOMPARE(api.queries.size(), 1);
        QCOMPARE(list->topLevelItemCount(), 1);
        QCOMPARE(list->topLevelItem(0)->text(1), QStringLiteral("keep-me"));
        QVERIFY(list->isEnabled());
        QVERIFY(window.findChild<QPushButton*>(QStringLiteral("preset_set"))->isEnabled());
        QVERIFY(window.findChild<QPushButton*>(QStringLiteral("preset_goto"))->isEnabled());
        QVERIFY(window.findChild<QPushButton*>(QStringLiteral("preset_remove"))->isEnabled());
        window.close();
    }

    void preset_confirmation_target_reset_data()
    {
        QTest::addColumn<QString>("action");
        QTest::addColumn<QString>("command");
        int index = 0;
        for (const auto& action : {QStringLiteral("a-to-b"),
                                   QStringLiteral("a-to-b-to-a"),
                                   QStringLiteral("refresh"),
                                   QStringLiteral("platform"),
                                   QStringLiteral("stop"),
                                   QStringLiteral("close"),
                                   QStringLiteral("destruct")})
        {
            QTest::newRow(action.toLatin1().constData())
                << action << (index++ % 2 == 0 ? QStringLiteral("preset_set") : QStringLiteral("preset_remove"));
        }
    }

    void preset_confirmation_target_reset()
    {
        QFETCH(QString, action);
        QFETCH(QString, command);
        platform_fixture api(true);
        QVERIFY(api.start());
        auto window = std::make_unique<monitor_window>();
        window->show();
        QVERIFY(select_gb_channel(*window, 1));
        QVERIFY(select_gb_channel(*window));
        QVERIFY(load_preset_rows(*window, api, {QJsonObject{{"preset_id", "1"}, {"preset_name", "target-A"}}}));
        auto* list = window->findChild<QTreeWidget*>(QStringLiteral("preset_list"));
        auto* tree = window->findChild<QTreeWidget*>(QStringLiteral("device_tree"));
        list->setCurrentItem(list->topLevelItem(0));
        window->findChild<QPushButton*>(command)->click();
        QPointer<QMessageBox> confirmation = window->findChild<QMessageBox*>(QStringLiteral("preset_confirm"));
        QPointer<monitor_window> guarded = window.get();
        QVERIFY(confirmation && confirmation->isVisible());
        QVERIFY(!window->findChild<QPushButton*>(QStringLiteral("device_info_query"))->isEnabled());
        QVERIFY(window->findChild<QPushButton*>(QStringLiteral("ptz_stop"))->isEnabled());
        platform_fixture second(true);
        if (action.startsWith(QStringLiteral("a-to-b")))
        {
            tree->setCurrentItem(tree->topLevelItem(0)->child(1)->child(0));
            if (action == QStringLiteral("a-to-b-to-a"))
            {
                tree->setCurrentItem(tree->topLevelItem(0)->child(0)->child(0));
            }
        }
        else if (action == QStringLiteral("destruct"))
        {
            window.reset();
        }
        else if (action == QStringLiteral("close"))
        {
            window->close();
        }
        else if (action == QStringLiteral("stop"))
        {
            window->findChild<QPushButton*>(QStringLiteral("ptz_stop"))->click();
        }
        else
        {
            if (action == QStringLiteral("platform"))
            {
                QVERIFY(second.start());
                window->findChild<QLineEdit*>(QStringLiteral("platform_address"))
                    ->setText(QStringLiteral("http://127.0.0.1:%1").arg(second.server.serverPort()));
            }
            window->findChild<QPushButton*>(QStringLiteral("refresh_devices"))->click();
        }
        QVERIFY(QTest::qWaitFor([&] { return confirmation.isNull(); }, 1000));
        if (action == QStringLiteral("stop"))
        {
            QVERIFY(QTest::qWaitFor([&] { return api.queries.size() == 2; }, 2000));
            QCOMPARE(api.queries[1].body, QJsonObject({{"command", "stop"}}));
            QVERIFY(!window->findChild<QPushButton*>(QStringLiteral("preset_set"))->isEnabled());
            respond(api.queries[1].socket, 200, QJsonDocument(ptz_response(QStringLiteral("stop"))).toJson());
            QVERIFY(QTest::qWaitFor([&] { return window->findChild<QPushButton*>(QStringLiteral("preset_set"))->isEnabled(); }, 2000));
        }
        QCOMPARE(api.queries.size(), action == QStringLiteral("stop") ? 2 : 1);
        QCOMPARE(second.queries.size(), 0);
        if (action == QStringLiteral("destruct"))
        {
            QVERIFY(guarded.isNull());
        }
        else
        {
            window.reset();
        }
    }

    void preset_control_invalid_response_data()
    {
        QTest::addColumn<int>("status");
        QTest::addColumn<QByteArray>("body");
        for (const auto& key :
             {QStringLiteral("device_id"), QStringLiteral("channel_id"), QStringLiteral("command"), QStringLiteral("physical_result")})
        {
            auto response = preset_control_response(QStringLiteral("preset_goto"), 1);
            response.insert(key, QStringLiteral("wrong"));
            QTest::newRow(key.toLatin1().constData()) << 200 << QJsonDocument(response).toJson();
        }
        for (const auto& entry : {std::pair<const char*, QJsonValue>{"id-string", QStringLiteral("1")},
                                  {"id-fraction", 1.5},
                                  {"id-mismatch", 2},
                                  {"id-missing", QJsonValue(QJsonValue::Undefined)}})
        {
            auto response = preset_control_response(QStringLiteral("preset_goto"), 1);
            response.insert(QStringLiteral("preset_id"), entry.second);
            QTest::newRow(entry.first) << 200 << QJsonDocument(response).toJson();
        }
        for (const auto& entry : {std::pair<const char*, QJsonValue>{"accepted-false", false},
                                  {"accepted-string", QStringLiteral("true")},
                                  {"accepted-missing", QJsonValue(QJsonValue::Undefined)}})
        {
            auto response = preset_control_response(QStringLiteral("preset_goto"), 1);
            response.insert(QStringLiteral("accepted"), entry.second);
            QTest::newRow(entry.first) << 200 << QJsonDocument(response).toJson();
        }
        QTest::newRow("unexpected-202") << 202 << QJsonDocument(preset_control_response(QStringLiteral("preset_goto"), 1)).toJson();
        QTest::newRow("invalid-json") << 200 << QByteArray("{broken");
        for (const int code : {400, 409, 502})
        {
            QTest::newRow(QByteArray::number(code).constData()) << code << QByteArray(R"({"error":"preset_failed"})");
        }
    }

    void preset_control_invalid_response()
    {
        QFETCH(int, status);
        QFETCH(QByteArray, body);
        platform_fixture api(true);
        QVERIFY(api.start());
        monitor_window window;
        QVERIFY(select_gb_channel(window));
        QVERIFY(load_preset_rows(window, api, {QJsonObject{{"preset_id", "1"}, {"preset_name", "keep-on-goto"}}}));
        auto* list = window.findChild<QTreeWidget*>(QStringLiteral("preset_list"));
        auto* go = window.findChild<QPushButton*>(QStringLiteral("preset_goto"));
        auto* result = window.findChild<QLabel*>(QStringLiteral("preset_result"));
        list->setCurrentItem(list->topLevelItem(0));
        go->click();
        QVERIFY(QTest::qWaitFor([&] { return api.queries.size() == 2; }, 2000));
        const auto reply = pending_ptz(window);
        QVERIFY(reply);
        respond(api.queries[1].socket, status, body);
        QVERIFY(QTest::qWaitFor([&] { return go->isEnabled(); }, 2000));
        QVERIFY2(result->text().contains(QStringLiteral("失败")) && result->text().contains(QStringLiteral("未确认")), qPrintable(result->text()));
        QCOMPARE(list->topLevelItemCount(), 1);
        QVERIFY(QTest::qWaitFor([&] { return reply.isNull(); }, 1000));
        window.close();
    }

    void preset_control_stop_preempts_reply_data()
    {
        QTest::addColumn<QString>("command");
        QTest::addColumn<int>("old_status");
        for (const auto& command : {QStringLiteral("preset_set"), QStringLiteral("preset_goto"), QStringLiteral("preset_remove")})
        {
            QTest::newRow((command + QStringLiteral("-success")).toLatin1().constData()) << command << 200;
            QTest::newRow((command + QStringLiteral("-error")).toLatin1().constData()) << command << 502;
        }
    }

    void preset_control_stop_preempts_reply()
    {
        QFETCH(QString, command);
        QFETCH(int, old_status);
        platform_fixture api(true);
        QVERIFY(api.start());
        monitor_window window;
        QVERIFY(select_gb_channel(window));
        QVERIFY(load_preset_rows(window, api, {QJsonObject{{"preset_id", "1"}, {"preset_name", "first"}}}));
        auto* list = window.findChild<QTreeWidget*>(QStringLiteral("preset_list"));
        auto* result = window.findChild<QLabel*>(QStringLiteral("preset_result"));
        auto* ptz_result = window.findChild<QLabel*>(QStringLiteral("ptz_result"));
        auto* stop = window.findChild<QPushButton*>(QStringLiteral("ptz_stop"));
        auto* set = window.findChild<QPushButton*>(QStringLiteral("preset_set"));
        list->setCurrentItem(list->topLevelItem(0));
        window.findChild<QPushButton*>(command)->click();
        if (command != QStringLiteral("preset_goto"))
        {
            auto* box = window.findChild<QMessageBox*>(QStringLiteral("preset_confirm"));
            QVERIFY(box);
            box->button(QMessageBox::Yes)->click();
        }
        QVERIFY(QTest::qWaitFor([&] { return api.queries.size() == 2; }, 2000));
        const auto old_reply = pending_ptz(window);
        QVERIFY(old_reply);
        QVERIFY(!set->isEnabled() && stop->isEnabled());
        stop->click();
        QVERIFY(QTest::qWaitFor([&] { return api.queries.size() == 3; }, 2000));
        QCOMPARE(api.queries[2].body, QJsonObject({{"command", "stop"}}));
        const auto waiting = ptz_result->text();
        respond(api.queries[1].socket,
                old_status,
                old_status == 200 ? QJsonDocument(preset_control_response(command, 1)).toJson() : QByteArray(R"({"error":"stale-preset"})"));
        QVERIFY(QTest::qWaitFor([&] { return old_reply.isNull(); }, 1000));
        QCOMPARE(ptz_result->text(), waiting);
        QVERIFY(result->text().isEmpty());
        QVERIFY(!set->isEnabled() && !list->isEnabled() && stop->isEnabled());
        respond(api.queries[2].socket, 200, QJsonDocument(ptz_response(QStringLiteral("stop"))).toJson());
        QVERIFY(QTest::qWaitFor([&] { return set->isEnabled(); }, 2000));
        QVERIFY(result->text().isEmpty());
        QCOMPARE(api.queries.size(), 3);
        window.close();
    }

    void preset_control_pending_reset_data()
    {
        QTest::addColumn<QString>("action");
        QTest::addColumn<int>("old_status");
        int index = 0;
        for (const auto& action : {QStringLiteral("a-to-b"),
                                   QStringLiteral("a-to-b-to-a"),
                                   QStringLiteral("refresh"),
                                   QStringLiteral("platform"),
                                   QStringLiteral("close"),
                                   QStringLiteral("destruct")})
        {
            QTest::newRow(action.toLatin1().constData()) << action << (index++ % 2 == 0 ? 200 : 502);
        }
    }

    void preset_control_pending_reset()
    {
        QFETCH(QString, action);
        QFETCH(int, old_status);
        platform_fixture api(true);
        QVERIFY(api.start());
        auto window = std::make_unique<monitor_window>();
        window->show();
        QVERIFY(select_gb_channel(*window, 1));
        QVERIFY(select_gb_channel(*window));
        QVERIFY(load_preset_rows(*window, api, {QJsonObject{{"preset_id", "1"}, {"preset_name", "old-A"}}}));
        auto* list = window->findChild<QTreeWidget*>(QStringLiteral("preset_list"));
        auto* tree = window->findChild<QTreeWidget*>(QStringLiteral("device_tree"));
        QPointer<QLabel> result = window->findChild<QLabel*>(QStringLiteral("preset_result"));
        list->setCurrentItem(list->topLevelItem(0));
        window->findChild<QPushButton*>(QStringLiteral("preset_goto"))->click();
        QVERIFY(QTest::qWaitFor([&] { return api.queries.size() == 2; }, 2000));
        const auto old_reply = pending_ptz(*window);
        const auto peer = api.queries[1].socket;
        QVERIFY(old_reply && peer);
        platform_fixture second(true);
        const bool selection = action.startsWith(QStringLiteral("a-to-b"));
        if (selection)
        {
            tree->setCurrentItem(tree->topLevelItem(0)->child(1)->child(0));
            if (action == QStringLiteral("a-to-b-to-a"))
            {
                tree->setCurrentItem(tree->topLevelItem(0)->child(0)->child(0));
            }
            window->findChild<QPushButton*>(QStringLiteral("ptz_left"))->click();
            QVERIFY(QTest::qWaitFor([&] { return api.queries.size() == 3; }, 2000));
            const auto target = action == QStringLiteral("a-to-b") ? QByteArray("34020000001320000002") : QByteArray("34020000001320000001");
            QCOMPARE(api.queries[2].path, QByteArray("/api/devices/") + target + "/channels/34020000001310000001/ptz");
        }
        else if (action == QStringLiteral("destruct"))
        {
            window.reset();
            QVERIFY(result.isNull() && old_reply.isNull());
        }
        else if (action == QStringLiteral("close"))
        {
            window->close();
        }
        else
        {
            if (action == QStringLiteral("platform"))
            {
                QVERIFY(second.start());
                window->findChild<QLineEdit*>(QStringLiteral("platform_address"))
                    ->setText(QStringLiteral("http://127.0.0.1:%1").arg(second.server.serverPort()));
            }
            window->findChild<QPushButton*>(QStringLiteral("refresh_devices"))->click();
        }
        const auto after_reset = result ? result->text() : QString{};
        if (peer && peer->state() == QAbstractSocket::ConnectedState)
        {
            respond(peer,
                    old_status,
                    old_status == 200 ? QJsonDocument(preset_control_response(QStringLiteral("preset_goto"), 1)).toJson()
                                      : QByteArray(R"({"error":"late-preset"})"));
        }
        QVERIFY(QTest::qWaitFor([&] { return old_reply.isNull() && peer.isNull(); }, 1000));
        QCOMPARE(api.queries.size(), selection ? 3 : 2);
        QCOMPARE(second.queries.size(), 0);
        if (!result)
        {
            return;
        }
        QCOMPARE(result->text(), after_reset);
        if (selection)
        {
            QVERIFY(!window->findChild<QPushButton*>(QStringLiteral("preset_set"))->isEnabled());
            const auto id = action == QStringLiteral("a-to-b") ? QStringLiteral("34020000001320000002") : QStringLiteral("34020000001320000001");
            respond(api.queries[2].socket, 200, QJsonDocument(ptz_response(QStringLiteral("left"), id)).toJson());
            QVERIFY(QTest::qWaitFor([&] { return window->findChild<QPushButton*>(QStringLiteral("preset_set"))->isEnabled(); }, 2000));
        }
        window.reset();
        QVERIFY(result.isNull());
    }

    void ptz_controls_exist()
    {
        platform_fixture api(true);
        QVERIFY(api.start());
        monitor_window window;
        auto* tree = window.findChild<QTreeWidget*>(QStringLiteral("device_tree"));
        auto* speed = window.findChild<QSpinBox*>(QStringLiteral("ptz_speed"));
        auto* zoom = window.findChild<QSpinBox*>(QStringLiteral("ptz_zoom_speed"));
        auto* result = window.findChild<QLabel*>(QStringLiteral("ptz_result"));
        auto* target = window.findChild<QLabel*>(QStringLiteral("device_query_target"));
        QVERIFY(tree && speed && zoom && result && target);
        QCOMPARE(speed->minimum(), 1);
        QCOMPARE(speed->maximum(), 255);
        QCOMPARE(speed->value(), 64);
        QCOMPARE(zoom->minimum(), 1);
        QCOMPARE(zoom->maximum(), 15);
        QCOMPARE(zoom->value(), 4);
        QCOMPARE(result->textFormat(), Qt::PlainText);
        for (const auto& command : ptz_commands)
        {
            auto* button = window.findChild<QPushButton*>(QStringLiteral("ptz_") + command);
            QVERIFY2(button, qPrintable(command));
            QVERIFY(!button->autoRepeat());
            QVERIFY(!button->isEnabled());
        }
        QVERIFY(QTest::qWaitFor(
            [&]
            {
                return tree->topLevelItemCount() == 3 && tree->topLevelItem(0)->childCount() == 2 && tree->topLevelItem(1)->childCount() == 1 &&
                       tree->topLevelItem(2)->childCount() == 3;
            },
            2000));
        for (auto* item : {tree->topLevelItem(0), tree->topLevelItem(0)->child(0), tree->topLevelItem(1)->child(0), tree->topLevelItem(2)->child(0)})
        {
            tree->setCurrentItem(item);
            for (const auto& command : ptz_commands)
            {
                auto* button = window.findChild<QPushButton*>(QStringLiteral("ptz_") + command);
                QVERIFY(!button->isEnabled());
                button->click();
            }
        }
        QCOMPARE(api.queries.size(), 0);
        auto* device = tree->topLevelItem(0)->child(0);
        QCOMPARE(target->text(), QStringLiteral("请选择国标设备或通道"));
        tree->setCurrentItem(device);
        QCOMPARE(target->text(), QStringLiteral("34020000001320000001"));
        tree->itemDoubleClicked(device, 0);
        QVERIFY(QTest::qWaitFor([&] { return device->childCount() == 1; }, 2000));
        tree->setCurrentItem(device->child(0));
        QCOMPARE(target->text(), QStringLiteral("34020000001320000001\n34020000001310000001"));
        for (const auto& command : ptz_commands)
        {
            QVERIFY(window.findChild<QPushButton*>(QStringLiteral("ptz_") + command)->isEnabled());
        }
        QVERIFY(speed->isEnabled() && zoom->isEnabled());
        window.close();
    }

    void ptz_parameters_and_acceptance()
    {
        platform_fixture api(true);
        QVERIFY(api.start());
        monitor_window window;
        auto* tree = window.findChild<QTreeWidget*>(QStringLiteral("device_tree"));
        auto* speed = window.findChild<QSpinBox*>(QStringLiteral("ptz_speed"));
        auto* zoom = window.findChild<QSpinBox*>(QStringLiteral("ptz_zoom_speed"));
        auto* result = window.findChild<QLabel*>(QStringLiteral("ptz_result"));
        auto* stop = window.findChild<QPushButton*>(QStringLiteral("ptz_stop"));
        QVERIFY(tree && speed && zoom && result && stop);
        QVERIFY(QTest::qWaitFor([&] { return tree->topLevelItemCount() == 3 && tree->topLevelItem(0)->childCount() == 2; }, 2000));
        auto* device = tree->topLevelItem(0)->child(0);
        tree->itemDoubleClicked(device, 0);
        QVERIFY(QTest::qWaitFor([&] { return device->childCount() == 1; }, 2000));
        tree->setCurrentItem(device->child(0));
        int count = 0;
        for (const auto& command : ptz_commands)
        {
            if (command == QStringLiteral("right"))
            {
                speed->setValue(255);
            }
            if (command == QStringLiteral("up"))
            {
                speed->setValue(1);
            }
            if (command == QStringLiteral("zoom_out"))
            {
                zoom->setValue(15);
            }
            auto* button = window.findChild<QPushButton*>(QStringLiteral("ptz_") + command);
            QVERIFY(button);
            button->click();
            ++count;
            QVERIFY(QTest::qWaitFor([&] { return api.queries.size() == count; }, 2000));
            const auto& request = api.queries.back();
            QCOMPARE(request.request_line, QByteArray("POST /api/devices/34020000001320000001/channels/34020000001310000001/ptz HTTP/1.1"));
            QJsonObject expected{{"command", command}};
            if (command != QStringLiteral("stop"))
            {
                expected.insert(QStringLiteral("speed"), command.startsWith(QStringLiteral("zoom_")) ? zoom->value() : speed->value());
            }
            QCOMPARE(request.body, expected);
            QVERIFY(stop->isEnabled());
            for (const auto& name : {QStringLiteral("device_info_query"),
                                     QStringLiteral("device_status_query"),
                                     QStringLiteral("preset_query"),
                                     QStringLiteral("ptz_left")})
            {
                QVERIFY(!window.findChild<QPushButton*>(name)->isEnabled());
            }
            const auto reply = pending_ptz(window);
            QVERIFY(reply);
            respond(request.socket, 200, QJsonDocument(ptz_response(command)).toJson(QJsonDocument::Compact));
            QVERIFY(QTest::qWaitFor([&] { return speed->isEnabled(); }, 2000));
            QVERIFY(result->text().contains(QStringLiteral("未确认")));
            QVERIFY(!result->text().contains(QStringLiteral("失败")));
            QVERIFY(QTest::qWaitFor([&] { return reply.isNull(); }, 1000));
        }
        QCOMPARE(api.queries.size(), ptz_commands.size());
        window.close();
    }

    void ptz_invalid_response_data()
    {
        QTest::addColumn<QString>("command");
        QTest::addColumn<int>("status");
        QTest::addColumn<QByteArray>("body");
        for (const auto& key : {QStringLiteral("device_id"),
                                QStringLiteral("channel_id"),
                                QStringLiteral("command"),
                                QStringLiteral("start"),
                                QStringLiteral("stop"),
                                QStringLiteral("physical_result")})
        {
            auto wrong = ptz_response(QStringLiteral("left"));
            wrong.insert(key, QStringLiteral("wrong"));
            QTest::newRow((QStringLiteral("wrong-") + key).toLatin1().constData()) << QStringLiteral("left") << 200 << QJsonDocument(wrong).toJson();
            auto missing = ptz_response(QStringLiteral("left"));
            missing.remove(key);
            QTest::newRow((QStringLiteral("missing-") + key).toLatin1().constData())
                << QStringLiteral("left") << 200 << QJsonDocument(missing).toJson();
        }
        QTest::newRow("invalid-json") << QStringLiteral("left") << 200 << QByteArray("{broken");
        QTest::newRow("motion-cannot-be-202") << QStringLiteral("left") << 202 << QJsonDocument(ptz_response(QStringLiteral("left"))).toJson();
        auto stop_wrong = ptz_response(QStringLiteral("stop"));
        stop_wrong.insert(QStringLiteral("stop"), QStringLiteral("accepted"));
        QTest::newRow("stop-202-must-be-requested") << QStringLiteral("stop") << 202 << QJsonDocument(stop_wrong).toJson();
        stop_wrong.insert(QStringLiteral("stop"), QStringLiteral("requested"));
        QTest::newRow("stop-200-must-be-accepted") << QStringLiteral("stop") << 200 << QJsonDocument(stop_wrong).toJson();
        for (const int code : {400, 409, 502})
        {
            QTest::newRow(QByteArray::number(code).constData()) << QStringLiteral("left") << code << QByteArray(R"({"error":"ptz_failed"})");
        }
    }

    void ptz_invalid_response()
    {
        QFETCH(QString, command);
        QFETCH(int, status);
        QFETCH(QByteArray, body);
        platform_fixture api(true);
        QVERIFY(api.start());
        monitor_window window;
        auto* tree = window.findChild<QTreeWidget*>(QStringLiteral("device_tree"));
        auto* result = window.findChild<QLabel*>(QStringLiteral("ptz_result"));
        auto* button = window.findChild<QPushButton*>(QStringLiteral("ptz_") + command);
        auto* info = window.findChild<QPushButton*>(QStringLiteral("device_info_query"));
        QVERIFY(tree && result && button && info);
        QVERIFY(QTest::qWaitFor([&] { return tree->topLevelItemCount() == 3 && tree->topLevelItem(0)->childCount() == 2; }, 2000));
        auto* device = tree->topLevelItem(0)->child(0);
        tree->itemDoubleClicked(device, 0);
        QVERIFY(QTest::qWaitFor([&] { return device->childCount() == 1; }, 2000));
        tree->setCurrentItem(device->child(0));
        button->click();
        QVERIFY(QTest::qWaitFor([&] { return api.queries.size() == 1; }, 2000));
        const auto reply = pending_ptz(window);
        QVERIFY(reply);
        respond(api.queries[0].socket, status, body);
        QVERIFY(QTest::qWaitFor([&] { return info->isEnabled(); }, 2000));
        QVERIFY2(result->text().contains(QStringLiteral("失败")), qPrintable(result->text()));
        QVERIFY(window.findChild<QPushButton*>(QStringLiteral("ptz_stop"))->isEnabled());
        QVERIFY(QTest::qWaitFor([&] { return reply.isNull(); }, 1000));
        window.close();
    }

    void ptz_stop_preempts_pending_data()
    {
        QTest::addColumn<QString>("old_command");
        QTest::addColumn<int>("old_status");
        for (const auto& command :
             {QStringLiteral("DeviceInfo"), QStringLiteral("DeviceStatus"), QStringLiteral("PresetQuery"), QStringLiteral("left")})
        {
            QTest::newRow((command + QStringLiteral("-success")).toLatin1().constData()) << command << 200;
            QTest::newRow((command + QStringLiteral("-error")).toLatin1().constData()) << command << 502;
        }
    }

    void ptz_stop_preempts_pending()
    {
        QFETCH(QString, old_command);
        QFETCH(int, old_status);
        platform_fixture api(true);
        QVERIFY(api.start());
        monitor_window window;
        auto* tree = window.findChild<QTreeWidget*>(QStringLiteral("device_tree"));
        auto* stop = window.findChild<QPushButton*>(QStringLiteral("ptz_stop"));
        auto* result = window.findChild<QLabel*>(QStringLiteral("ptz_result"));
        auto* info_result = window.findChild<QPlainTextEdit*>(QStringLiteral("device_query_result"));
        auto* preset_result = window.findChild<QLabel*>(QStringLiteral("preset_result"));
        auto* info = window.findChild<QPushButton*>(QStringLiteral("device_info_query"));
        QVERIFY(tree && stop && result && info_result && preset_result && info);
        QVERIFY(QTest::qWaitFor([&] { return tree->topLevelItemCount() == 3 && tree->topLevelItem(0)->childCount() == 2; }, 2000));
        auto* device = tree->topLevelItem(0)->child(0);
        tree->itemDoubleClicked(device, 0);
        QVERIFY(QTest::qWaitFor([&] { return device->childCount() == 1; }, 2000));
        tree->setCurrentItem(device->child(0));
        const auto name = old_command == QStringLiteral("DeviceInfo")     ? QStringLiteral("device_info_query")
                          : old_command == QStringLiteral("DeviceStatus") ? QStringLiteral("device_status_query")
                          : old_command == QStringLiteral("PresetQuery")  ? QStringLiteral("preset_query")
                                                                          : QStringLiteral("ptz_left");
        window.findChild<QPushButton*>(name)->click();
        QVERIFY(QTest::qWaitFor([&] { return api.queries.size() == 1; }, 2000));
        const auto old_reply = old_command == QStringLiteral("left") ? pending_ptz(window) : pending_device_query(window);
        QVERIFY(old_reply);
        QVERIFY(stop->isEnabled() && !info->isEnabled());
        window.findChild<QPushButton*>(QStringLiteral("ptz_right"))->click();
        QCOMPARE(api.queries.size(), 1);
        stop->click();
        QVERIFY(QTest::qWaitFor([&] { return api.queries.size() == 2; }, 2000));
        QCOMPARE(api.queries[1].body, QJsonObject({{"command", "stop"}}));
        QVERIFY(info_result->toPlainText().isEmpty());
        QVERIFY(!preset_result->text().contains(QStringLiteral("等待")));
        const auto waiting = result->text();
        const auto old_body =
            old_command == QStringLiteral("PresetQuery")
                ? QJsonDocument(preset_query_response(QStringLiteral("34020000001320000001"),
                                                      QStringLiteral("34020000001310000001"),
                                                      {QJsonObject{{"preset_id", "stale-control"}, {"preset_name", "stale-control"}}}))
                      .toJson()
            : old_command == QStringLiteral("left")
                ? QJsonDocument(ptz_response(old_command)).toJson()
                : device_query_response(QStringLiteral("34020000001320000001"), old_command, QStringLiteral("stale-control"));
        respond(api.queries[0].socket, old_status, old_status == 200 ? old_body : QByteArray(R"({"error":"stale-control"})"));
        QVERIFY(QTest::qWaitFor([&] { return old_reply.isNull(); }, 1000));
        QCOMPARE(result->text(), waiting);
        QVERIFY(!info->isEnabled());
        QVERIFY(stop->isEnabled());
        QVERIFY(info_result->toPlainText().isEmpty());
        QCOMPARE(window.findChild<QTreeWidget*>(QStringLiteral("preset_list"))->topLevelItemCount(), 0);
        auto response = ptz_response(QStringLiteral("stop"));
        response.insert(QStringLiteral("stop"), QStringLiteral("requested"));
        respond(api.queries[1].socket, 202, QJsonDocument(response).toJson());
        QVERIFY(QTest::qWaitFor([&] { return info->isEnabled(); }, 2000));
        QVERIFY2(result->text().contains(QStringLiteral("未确认")) && !result->text().contains(QStringLiteral("失败")), qPrintable(result->text()));
        QVERIFY(result->text().contains(QStringLiteral("已请求停止")) && result->text().contains(QStringLiteral("日志")));
        QCOMPARE(api.queries.size(), 2);
        window.close();
    }

    void ptz_repeated_stop_keeps_latest_result()
    {
        platform_fixture api(true);
        QVERIFY(api.start());
        monitor_window window;
        auto* tree = window.findChild<QTreeWidget*>(QStringLiteral("device_tree"));
        auto* stop = window.findChild<QPushButton*>(QStringLiteral("ptz_stop"));
        auto* result = window.findChild<QLabel*>(QStringLiteral("ptz_result"));
        auto* left = window.findChild<QPushButton*>(QStringLiteral("ptz_left"));
        QVERIFY(tree && stop && result && left);
        QVERIFY(QTest::qWaitFor([&] { return tree->topLevelItemCount() == 3 && tree->topLevelItem(0)->childCount() == 2; }, 2000));
        auto* device = tree->topLevelItem(0)->child(0);
        tree->itemDoubleClicked(device, 0);
        QVERIFY(QTest::qWaitFor([&] { return device->childCount() == 1; }, 2000));
        tree->setCurrentItem(device->child(0));
        stop->click();
        QVERIFY(QTest::qWaitFor([&] { return api.queries.size() == 1; }, 2000));
        const auto first = pending_ptz(window);
        QVERIFY(first);
        respond(api.queries[0].socket, 502, R"({"error":"stale-first-stop"})");
        stop->click();
        QVERIFY(QTest::qWaitFor([&] { return api.queries.size() == 2; }, 2000));
        QVERIFY(QTest::qWaitFor([&] { return first.isNull(); }, 1000));
        QVERIFY(!left->isEnabled() && stop->isEnabled());
        QVERIFY(!result->text().contains(QStringLiteral("stale-first-stop")) && !result->text().contains(QStringLiteral("失败")));
        QCOMPARE(api.queries[0].body, QJsonObject({{"command", "stop"}}));
        QCOMPARE(api.queries[1].body, QJsonObject({{"command", "stop"}}));
        respond(api.queries[1].socket, 200, QJsonDocument(ptz_response(QStringLiteral("stop"))).toJson());
        QVERIFY(QTest::qWaitFor([&] { return left->isEnabled(); }, 2000));
        QVERIFY(result->text().contains(QStringLiteral("未确认")) && !result->text().contains(QStringLiteral("失败")));
        window.close();
    }

    void ptz_target_reset_isolates_late_reply_data()
    {
        QTest::addColumn<QString>("action");
        QTest::addColumn<QString>("command");
        QTest::addColumn<int>("old_status");
        for (const auto& action : {QStringLiteral("a-to-b"),
                                   QStringLiteral("a-to-b-to-a"),
                                   QStringLiteral("refresh"),
                                   QStringLiteral("platform"),
                                   QStringLiteral("close"),
                                   QStringLiteral("destruct")})
        {
            QTest::newRow((action + QStringLiteral("-motion-success")).toLatin1().constData()) << action << QStringLiteral("left") << 200;
            QTest::newRow((action + QStringLiteral("-stop-error")).toLatin1().constData()) << action << QStringLiteral("stop") << 502;
        }
        QTest::newRow("a-to-b-to-a-stop-success") << QStringLiteral("a-to-b-to-a") << QStringLiteral("stop") << 200;
        QTest::newRow("close-motion-error") << QStringLiteral("close") << QStringLiteral("left") << 502;
    }

    void ptz_target_reset_isolates_late_reply()
    {
        QFETCH(QString, action);
        QFETCH(QString, command);
        QFETCH(int, old_status);
        platform_fixture api(true);
        QVERIFY(api.start());
        auto window = std::make_unique<monitor_window>();
        window->show();
        auto* tree = window->findChild<QTreeWidget*>(QStringLiteral("device_tree"));
        auto* refresh = window->findChild<QPushButton*>(QStringLiteral("refresh_devices"));
        auto* address = window->findChild<QLineEdit*>(QStringLiteral("platform_address"));
        QPointer<QLabel> result = window->findChild<QLabel*>(QStringLiteral("ptz_result"));
        auto* stop = window->findChild<QPushButton*>(QStringLiteral("ptz_stop"));
        auto* left = window->findChild<QPushButton*>(QStringLiteral("ptz_left"));
        QVERIFY(tree && refresh && address && result && stop && left);
        QVERIFY(QTest::qWaitFor([&] { return tree->topLevelItemCount() == 3 && tree->topLevelItem(0)->childCount() == 2; }, 2000));
        auto* a = tree->topLevelItem(0)->child(0);
        auto* b = tree->topLevelItem(0)->child(1);
        tree->itemDoubleClicked(a, 0);
        tree->itemDoubleClicked(b, 0);
        QVERIFY(QTest::qWaitFor([&] { return a->childCount() == 1 && b->childCount() == 1; }, 2000));
        tree->setCurrentItem(a->child(0));
        window->findChild<QPushButton*>(QStringLiteral("ptz_") + command)->click();
        QVERIFY(QTest::qWaitFor([&] { return api.queries.size() == 1; }, 2000));
        const auto reply = pending_ptz(*window);
        const auto peer = api.queries[0].socket;
        QVERIFY(reply && peer);
        platform_fixture second(true);
        const bool selection = action.startsWith(QStringLiteral("a-to-b"));
        QString target;
        if (selection)
        {
            tree->setCurrentItem(b->child(0));
            target = QStringLiteral("34020000001320000002");
            if (action == QStringLiteral("a-to-b-to-a"))
            {
                tree->setCurrentItem(a->child(0));
                target = QStringLiteral("34020000001320000001");
            }
            left->click();
            QVERIFY(QTest::qWaitFor([&] { return api.queries.size() == 2; }, 2000));
            QCOMPARE(api.queries[1].path, QByteArray("/api/devices/") + target.toLatin1() + "/channels/34020000001310000001/ptz");
        }
        else if (action == QStringLiteral("destruct"))
        {
            window.reset();
            QVERIFY(result.isNull() && reply.isNull());
        }
        else if (action == QStringLiteral("close"))
        {
            window->close();
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
        const auto after_reset = result ? result->text() : QString{};
        if (peer && peer->state() == QAbstractSocket::ConnectedState)
        {
            respond(
                peer, old_status, old_status == 200 ? QJsonDocument(ptz_response(command)).toJson() : QByteArray(R"({"error":"stale-reset-ptz"})"));
        }
        QVERIFY(QTest::qWaitFor([&] { return reply.isNull() && peer.isNull(); }, 1000));
        QCOMPARE(api.queries.size(), selection ? 2 : 1);
        QCOMPARE(second.queries.size(), 0);
        if (!result)
        {
            return;
        }
        QCOMPARE(result->text(), after_reset);
        QVERIFY(!result->text().contains(QStringLiteral("stale-reset-ptz")));
        if (selection)
        {
            QVERIFY(!left->isEnabled() && stop->isEnabled());
            respond(api.queries[1].socket, 200, QJsonDocument(ptz_response(QStringLiteral("left"), target)).toJson());
            QVERIFY(QTest::qWaitFor([&] { return left->isEnabled(); }, 2000));
            QVERIFY(result->text().contains(QStringLiteral("未确认")) && !result->text().contains(QStringLiteral("失败")));
        }
        else if (action != QStringLiteral("close"))
        {
            QVERIFY(!stop->isEnabled());
        }
        else
        {
            QVERIFY(!window->isVisible());
        }
        window.reset();
        QVERIFY(result.isNull());
    }

    void init()
    {
        settings_ = std::make_unique<QTemporaryDir>();
        QVERIFY(settings_->isValid());
        QCoreApplication::setOrganizationName(QStringLiteral("media-server-qt-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("monitor-window-stage2"));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_->path());
    }

    void preset_query_controls()
    {
        platform_fixture api(true);
        QVERIFY(api.start());
        monitor_window window;
        auto* tree = window.findChild<QTreeWidget*>(QStringLiteral("device_tree"));
        auto* query = window.findChild<QPushButton*>(QStringLiteral("preset_query"));
        auto* list = window.findChild<QTreeWidget*>(QStringLiteral("preset_list"));
        auto* result = window.findChild<QLabel*>(QStringLiteral("preset_result"));
        QVERIFY(tree && query && list && result);
        QCOMPARE(list->columnCount(), 2);
        QTRY_VERIFY_WITH_TIMEOUT(tree->topLevelItemCount() == 3 && tree->topLevelItem(0)->childCount() == 2 &&
                                     tree->topLevelItem(1)->childCount() == 1 && tree->topLevelItem(2)->childCount() == 3,
                                 2000);
        QVERIFY(!query->isEnabled());
        for (auto* item : {tree->topLevelItem(0), tree->topLevelItem(0)->child(0), tree->topLevelItem(1)->child(0), tree->topLevelItem(2)->child(0)})
        {
            tree->setCurrentItem(item);
            QVERIFY(!query->isEnabled());
            query->click();
        }
        QCOMPARE(api.queries.size(), 0);
        auto* device = tree->topLevelItem(0)->child(0);
        tree->itemDoubleClicked(device, 0);
        QTRY_COMPARE_WITH_TIMEOUT(device->childCount(), 1, 2000);
        tree->setCurrentItem(device->child(0));
        QVERIFY(query->isEnabled());
        query->click();
        QTRY_COMPARE_WITH_TIMEOUT(api.queries.size(), 1, 2000);
        QCOMPARE(api.queries[0].request_line,
                 QByteArray("POST /api/devices/34020000001320000001/channels/34020000001310000001/presets/query HTTP/1.1"));
        QCOMPARE(api.queries[0].body, QJsonObject{});
        const QStringList identifiers{QStringLiteral("camera-A"), QStringLiteral("1000"), QStringLiteral("中文编号")};
        const QStringList names{QStringLiteral("<b>literal-preset-name</b>"), QString{}, QStringLiteral("门口")};
        QJsonArray entries;
        for (int index = 0; index < identifiers.size(); ++index)
        {
            entries.append(QJsonObject{{"preset_id", identifiers[index]}, {"preset_name", names[index]}});
        }
        const auto response = preset_query_response(QStringLiteral("34020000001320000001"), QStringLiteral("34020000001310000001"), entries);
        QVERIFY(!response.contains(QStringLiteral("result")));
        respond(api.queries[0].socket, 200, QJsonDocument(response).toJson(QJsonDocument::Compact));
        QTRY_VERIFY_WITH_TIMEOUT(query->isEnabled(), 1000);
        QCOMPARE(list->topLevelItemCount(), 3);
        for (int index = 0; index < identifiers.size(); ++index)
        {
            QCOMPARE(list->topLevelItem(index)->text(0), identifiers[index]);
            QCOMPARE(list->topLevelItem(index)->text(1), names[index]);
            QVERIFY(!(list->topLevelItem(index)->flags() & Qt::ItemIsEditable));
        }
        QVERIFY(result->text().contains(QStringLiteral("预置位")) && result->text().contains(QStringLiteral("34020000001310000001")));
        query->click();
        QTRY_COMPARE_WITH_TIMEOUT(api.queries.size(), 2, 2000);
        respond(api.queries[1].socket,
                200,
                QJsonDocument(preset_query_response(QStringLiteral("34020000001320000001"), QStringLiteral("34020000001310000001"), {}))
                    .toJson(QJsonDocument::Compact));
        QTRY_VERIFY_WITH_TIMEOUT(query->isEnabled(), 1000);
        QCOMPARE(list->topLevelItemCount(), 0);
        QVERIFY(result->text().contains(QStringLiteral("预置位")) && !result->text().contains(QStringLiteral("失败")));
        tree->setCurrentItem(device);
        QVERIFY(!query->isEnabled());
        QCOMPARE(list->topLevelItemCount(), 0);
        window.close();
    }

    void preset_query_serializes_other_queries_data()
    {
        QTest::addColumn<QString>("command");
        QTest::newRow("info") << QStringLiteral("DeviceInfo");
        QTest::newRow("status") << QStringLiteral("DeviceStatus");
    }

    void preset_query_serializes_other_queries()
    {
        QFETCH(QString, command);
        platform_fixture api(true);
        QVERIFY(api.start());
        monitor_window window;
        auto* tree = window.findChild<QTreeWidget*>(QStringLiteral("device_tree"));
        auto* query = window.findChild<QPushButton*>(QStringLiteral("preset_query"));
        auto* info = window.findChild<QPushButton*>(QStringLiteral("device_info_query"));
        auto* status = window.findChild<QPushButton*>(QStringLiteral("device_status_query"));
        QVERIFY(tree && query && info && status);
        QTRY_VERIFY_WITH_TIMEOUT(tree->topLevelItemCount() == 3 && tree->topLevelItem(0)->childCount() == 2, 2000);
        auto* device = tree->topLevelItem(0)->child(0);
        tree->itemDoubleClicked(device, 0);
        QTRY_COMPARE_WITH_TIMEOUT(device->childCount(), 1, 2000);
        tree->setCurrentItem(device->child(0));
        (command == QStringLiteral("DeviceInfo") ? info : status)->click();
        QVERIFY(!info->isEnabled() && !status->isEnabled() && !query->isEnabled());
        query->click();
        QTRY_COMPARE_WITH_TIMEOUT(api.queries.size(), 1, 2000);
        respond(api.queries[0].socket, 200, device_query_response(QStringLiteral("34020000001320000001"), command, QStringLiteral("ordinary-query")));
        QTRY_VERIFY_WITH_TIMEOUT(info->isEnabled() && status->isEnabled() && query->isEnabled(), 1000);
        query->click();
        QVERIFY(!info->isEnabled() && !status->isEnabled() && !query->isEnabled());
        info->click();
        status->click();
        query->click();
        QTRY_COMPARE_WITH_TIMEOUT(api.queries.size(), 2, 2000);
        QVERIFY(api.queries[1].path.endsWith("/presets/query"));
        respond(api.queries[1].socket,
                200,
                QJsonDocument(preset_query_response(QStringLiteral("34020000001320000001"), QStringLiteral("34020000001310000001"), {}))
                    .toJson(QJsonDocument::Compact));
        QTRY_VERIFY_WITH_TIMEOUT(info->isEnabled() && status->isEnabled() && query->isEnabled(), 1000);
        QCOMPARE(api.queries.size(), 2);
        window.close();
    }

    void preset_query_invalid_response_data()
    {
        QTest::addColumn<int>("http_status");
        QTest::addColumn<QByteArray>("body");
        const auto valid = preset_query_response(QStringLiteral("34020000001320000001"),
                                                 QStringLiteral("34020000001310000001"),
                                                 {QJsonObject{{"preset_id", "first-valid"}, {"preset_name", "first"}}});
        auto variant = valid;
        const auto row = [](const char* name, const QJsonObject& response)
        { QTest::newRow(name) << 200 << QJsonDocument(response).toJson(QJsonDocument::Compact); };
        variant["device_id"] = "wrong-device";
        row("wrong-device", variant);
        variant = valid;
        variant["channel_id"] = "wrong-channel";
        row("wrong-channel", variant);
        variant = valid;
        variant["command"] = "DeviceInfo";
        row("wrong-command", variant);
        variant = valid;
        variant.remove("presets");
        row("missing-list", variant);
        variant = valid;
        variant["presets"] = QJsonObject{};
        row("list-not-array", variant);
        const char* invalid_names[] = {"entry-not-object", "numeric-id", "missing-id", "empty-id", "numeric-name", "missing-name"};
        int invalid_index = 0;
        for (const auto& entry : {QJsonValue(17),
                                  QJsonValue(QJsonObject{{"preset_id", 1000}, {"preset_name", "numeric-id"}}),
                                  QJsonValue(QJsonObject{{"preset_name", "missing-id"}}),
                                  QJsonValue(QJsonObject{{"preset_id", ""}, {"preset_name", "empty-id"}}),
                                  QJsonValue(QJsonObject{{"preset_id", "second"}, {"preset_name", 7}}),
                                  QJsonValue(QJsonObject{{"preset_id", "second"}})})
        {
            variant = valid;
            auto entries = valid.value("presets").toArray();
            entries.append(entry);
            variant["presets"] = entries;
            // 每一项在已合法首项之后，保证错误不能留下半个列表。
            row(invalid_names[invalid_index++], variant);
        }
        variant = valid;
        auto entries = valid.value("presets").toArray();
        entries.append(QJsonObject{{"preset_id", "first-valid"}, {"preset_name", "duplicate"}});
        variant["presets"] = entries;
        row("duplicate-id", variant);
        QTest::newRow("http-400") << 400 << QByteArray(R"({"error":"invalid_request"})");
        QTest::newRow("malformed-json") << 200 << QByteArray("{broken");
    }

    void preset_query_invalid_response()
    {
        QFETCH(int, http_status);
        QFETCH(QByteArray, body);
        platform_fixture api(true);
        QVERIFY(api.start());
        monitor_window window;
        auto* tree = window.findChild<QTreeWidget*>(QStringLiteral("device_tree"));
        auto* query = window.findChild<QPushButton*>(QStringLiteral("preset_query"));
        auto* list = window.findChild<QTreeWidget*>(QStringLiteral("preset_list"));
        auto* result = window.findChild<QLabel*>(QStringLiteral("preset_result"));
        auto* info = window.findChild<QPushButton*>(QStringLiteral("device_info_query"));
        auto* status = window.findChild<QPushButton*>(QStringLiteral("device_status_query"));
        QVERIFY(tree && query && list && result && info && status);
        QTRY_VERIFY_WITH_TIMEOUT(tree->topLevelItemCount() == 3 && tree->topLevelItem(0)->childCount() == 2, 2000);
        auto* device = tree->topLevelItem(0)->child(0);
        tree->itemDoubleClicked(device, 0);
        QTRY_COMPARE_WITH_TIMEOUT(device->childCount(), 1, 2000);
        tree->setCurrentItem(device->child(0));
        query->click();
        QTRY_COMPARE_WITH_TIMEOUT(api.queries.size(), 1, 2000);
        const auto reply = pending_device_query(window);
        QVERIFY(reply);
        respond(api.queries[0].socket, http_status, body);
        QTRY_VERIFY_WITH_TIMEOUT(query->isEnabled() && info->isEnabled() && status->isEnabled(), 1000);
        QVERIFY2(result->text().contains(QStringLiteral("预置位")) && result->text().contains(QStringLiteral("失败")), qPrintable(result->text()));
        QCOMPARE(list->topLevelItemCount(), 0);
        QTRY_VERIFY_WITH_TIMEOUT(reply.isNull(), 1000);
        window.close();
    }

    void preset_query_selection_isolates_late_reply_data()
    {
        QTest::addColumn<bool>("return_to_a");
        QTest::addColumn<int>("old_http_status");
        QTest::newRow("a-to-b-old-success") << false << 200;
        QTest::newRow("a-to-b-old-error") << false << 400;
        QTest::newRow("a-to-b-to-a-old-success") << true << 200;
        QTest::newRow("a-to-b-to-a-old-error") << true << 400;
    }

    void preset_query_selection_isolates_late_reply()
    {
        QFETCH(bool, return_to_a);
        QFETCH(int, old_http_status);
        platform_fixture api(true);
        QVERIFY(api.start());
        monitor_window window;
        auto* tree = window.findChild<QTreeWidget*>(QStringLiteral("device_tree"));
        auto* query = window.findChild<QPushButton*>(QStringLiteral("preset_query"));
        auto* list = window.findChild<QTreeWidget*>(QStringLiteral("preset_list"));
        auto* result = window.findChild<QLabel*>(QStringLiteral("preset_result"));
        auto* info = window.findChild<QPushButton*>(QStringLiteral("device_info_query"));
        auto* status = window.findChild<QPushButton*>(QStringLiteral("device_status_query"));
        QVERIFY(tree && query && list && result && info && status);
        QTRY_VERIFY_WITH_TIMEOUT(tree->topLevelItemCount() == 3 && tree->topLevelItem(0)->childCount() == 2, 2000);
        auto* a = tree->topLevelItem(0)->child(0);
        auto* b = tree->topLevelItem(0)->child(1);
        tree->itemDoubleClicked(a, 0);
        tree->itemDoubleClicked(b, 0);
        QTRY_VERIFY_WITH_TIMEOUT(a->childCount() == 1 && b->childCount() == 1, 2000);
        tree->setCurrentItem(a->child(0));
        query->click();
        QTRY_COMPARE_WITH_TIMEOUT(api.queries.size(), 1, 2000);
        const auto old_reply = pending_device_query(window);
        QVERIFY(old_reply);
        respond(api.queries[0].socket,
                old_http_status,
                old_http_status == 200
                    ? QJsonDocument(preset_query_response(QStringLiteral("34020000001320000001"),
                                                          QStringLiteral("34020000001310000001"),
                                                          {QJsonObject{{"preset_id", "stale-preset"}, {"preset_name", "stale-preset"}}}))
                          .toJson(QJsonDocument::Compact)
                    : QByteArray(R"({"error":"stale-preset"})"));
        tree->setCurrentItem(b->child(0));
        if (return_to_a)
        {
            tree->setCurrentItem(a->child(0));
        }
        QCOMPARE(list->topLevelItemCount(), 0);
        const auto device = return_to_a ? QStringLiteral("34020000001320000001") : QStringLiteral("34020000001320000002");
        query->click();
        QTRY_COMPARE_WITH_TIMEOUT(api.queries.size(), 2, 2000);
        QTRY_VERIFY_WITH_TIMEOUT(old_reply.isNull(), 1000);
        QVERIFY(!query->isEnabled() && !info->isEnabled() && !status->isEnabled());
        QCOMPARE(list->topLevelItemCount(), 0);
        QVERIFY(!result->text().contains(QStringLiteral("stale-preset")) && !result->text().contains(QStringLiteral("失败")));
        QCOMPARE(api.queries[1].path, QByteArray("/api/devices/") + device.toLatin1() + "/channels/34020000001310000001/presets/query");
        respond(api.queries[1].socket,
                200,
                QJsonDocument(preset_query_response(device,
                                                    QStringLiteral("34020000001310000001"),
                                                    {QJsonObject{{"preset_id", "fresh-preset"}, {"preset_name", "fresh-preset"}}}))
                    .toJson(QJsonDocument::Compact));
        QTRY_VERIFY_WITH_TIMEOUT(query->isEnabled(), 1000);
        QCOMPARE(list->topLevelItemCount(), 1);
        QCOMPARE(list->topLevelItem(0)->text(0), QStringLiteral("fresh-preset"));
        QVERIFY(!result->text().contains(QStringLiteral("stale-preset")));
        window.close();
    }

    void preset_query_reset_cancels_callback_data()
    {
        QTest::addColumn<QString>("action");
        QTest::addColumn<int>("old_http_status");
        for (const auto& action : {QStringLiteral("refresh"), QStringLiteral("platform"), QStringLiteral("close"), QStringLiteral("destruct")})
        {
            QTest::newRow((action + QStringLiteral("-success")).toLatin1().constData()) << action << 200;
            QTest::newRow((action + QStringLiteral("-error")).toLatin1().constData()) << action << 400;
        }
    }

    void preset_query_reset_cancels_callback()
    {
        QFETCH(QString, action);
        QFETCH(int, old_http_status);
        platform_fixture api(true);
        QVERIFY(api.start());
        auto window = std::make_unique<monitor_window>();
        window->show();
        auto* tree = window->findChild<QTreeWidget*>(QStringLiteral("device_tree"));
        auto* query = window->findChild<QPushButton*>(QStringLiteral("preset_query"));
        QPointer<QTreeWidget> list = window->findChild<QTreeWidget*>(QStringLiteral("preset_list"));
        QPointer<QLabel> result = window->findChild<QLabel*>(QStringLiteral("preset_result"));
        auto* refresh = window->findChild<QPushButton*>(QStringLiteral("refresh_devices"));
        auto* address = window->findChild<QLineEdit*>(QStringLiteral("platform_address"));
        QVERIFY(tree && query && list && result && refresh && address);
        QTRY_VERIFY_WITH_TIMEOUT(tree->topLevelItemCount() == 3 && tree->topLevelItem(0)->childCount() == 2, 2000);
        auto* device = tree->topLevelItem(0)->child(0);
        tree->itemDoubleClicked(device, 0);
        QTRY_COMPARE_WITH_TIMEOUT(device->childCount(), 1, 2000);
        tree->setCurrentItem(device->child(0));
        query->click();
        QTRY_COMPARE_WITH_TIMEOUT(api.queries.size(), 1, 2000);
        const auto old_reply = pending_device_query(*window);
        const auto peer = api.queries[0].socket;
        QVERIFY(old_reply && peer);
        respond(peer,
                old_http_status,
                old_http_status == 200
                    ? QJsonDocument(preset_query_response(QStringLiteral("34020000001320000001"),
                                                          QStringLiteral("34020000001310000001"),
                                                          {QJsonObject{{"preset_id", "late-preset"}, {"preset_name", "late-preset"}}}))
                          .toJson(QJsonDocument::Compact)
                    : QByteArray(R"({"error":"late-preset"})"));
        platform_fixture second(true);
        if (action == QStringLiteral("destruct"))
        {
            window.reset();
            QVERIFY(list.isNull() && result.isNull());
            QTRY_VERIFY_WITH_TIMEOUT(old_reply.isNull() && peer.isNull(), 1000);
            return;
        }
        if (action == QStringLiteral("close"))
        {
            window->close();
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
        const auto after_reset = result->text();
        QTRY_VERIFY_WITH_TIMEOUT(old_reply.isNull() && peer.isNull(), 1000);
        QCOMPARE(list->topLevelItemCount(), 0);
        QCOMPARE(result->text(), after_reset);
        QVERIFY(!result->text().contains(QStringLiteral("late-preset")));
        if (action != QStringLiteral("close"))
        {
            QVERIFY(!query->isEnabled());
            QTRY_VERIFY_WITH_TIMEOUT(tree->topLevelItemCount() == 3 && tree->topLevelItem(0)->childCount() == 2, 2000);
            auto* fresh_device = tree->topLevelItem(0)->child(0);
            tree->itemDoubleClicked(fresh_device, 0);
            QTRY_COMPARE_WITH_TIMEOUT(fresh_device->childCount(), 1, 2000);
            tree->setCurrentItem(fresh_device->child(0));
            QVERIFY(query->isEnabled());
            window->close();
        }
        else
        {
            QVERIFY(!window->isVisible());
        }
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
        // 保留原Info超时断言；另一个独立窗口在同一10秒内验证PresetQuery超时。
        platform_fixture preset_api(true);
        std::unique_ptr<monitor_window> preset_window;
        QPointer<QNetworkReply> preset_reply;
        QPointer<QTcpSocket> preset_peer;
        QPointer<QPushButton> preset_button;
        QPointer<QLabel> preset_result;
        platform_fixture ptz_api(true);
        std::unique_ptr<monitor_window> ptz_window;
        QPointer<QNetworkReply> ptz_reply;
        QPointer<QTcpSocket> ptz_peer;
        QPointer<QPushButton> ptz_button;
        QPointer<QLabel> ptz_result;
        platform_fixture preset_control_api(true);
        std::unique_ptr<monitor_window> preset_control_window;
        QPointer<QNetworkReply> preset_control_reply;
        QPointer<QTcpSocket> preset_control_peer;
        QPointer<QPushButton> preset_control_button;
        QPointer<QLabel> preset_control_result;
        if (http_status == 0)
        {
            const auto original_address = window.findChild<QLineEdit*>(QStringLiteral("platform_address"))->text();
            QVERIFY(preset_api.start());
            preset_window = std::make_unique<monitor_window>();
            auto* preset_tree = preset_window->findChild<QTreeWidget*>(QStringLiteral("device_tree"));
            preset_button = preset_window->findChild<QPushButton*>(QStringLiteral("preset_query"));
            preset_result = preset_window->findChild<QLabel*>(QStringLiteral("preset_result"));
            QVERIFY(preset_tree && preset_button && preset_result);
            QTRY_VERIFY_WITH_TIMEOUT(preset_tree->topLevelItemCount() == 3 && preset_tree->topLevelItem(0)->childCount() == 2, 2000);
            auto* device = preset_tree->topLevelItem(0)->child(0);
            preset_tree->itemDoubleClicked(device, 0);
            QTRY_COMPARE_WITH_TIMEOUT(device->childCount(), 1, 2000);
            preset_tree->setCurrentItem(device->child(0));
            preset_button->click();
            QTRY_COMPARE_WITH_TIMEOUT(preset_api.queries.size(), 1, 2000);
            preset_reply = pending_device_query(*preset_window);
            preset_peer = preset_api.queries[0].socket;
            QVERIFY(preset_reply && preset_peer);
            QVERIFY(preset_api.queries[0].path.endsWith("/presets/query"));
            QCOMPARE(window.findChild<QLineEdit*>(QStringLiteral("platform_address"))->text(), original_address);
            QCOMPARE(reply->url().port(), int(api.server.serverPort()));
            QCOMPARE(preset_reply->url().port(), int(preset_api.server.serverPort()));
            // 第三个隔离窗口复用同一真实超时窗口，不新增串行10秒等待。
            QVERIFY(ptz_api.start());
            ptz_window = std::make_unique<monitor_window>();
            auto* ptz_tree = ptz_window->findChild<QTreeWidget*>(QStringLiteral("device_tree"));
            ptz_button = ptz_window->findChild<QPushButton*>(QStringLiteral("ptz_left"));
            ptz_result = ptz_window->findChild<QLabel*>(QStringLiteral("ptz_result"));
            QVERIFY(ptz_tree && ptz_button && ptz_result);
            QVERIFY(QTest::qWaitFor([&] { return ptz_tree->topLevelItemCount() == 3 && ptz_tree->topLevelItem(0)->childCount() == 2; }, 2000));
            auto* ptz_device = ptz_tree->topLevelItem(0)->child(0);
            ptz_tree->itemDoubleClicked(ptz_device, 0);
            QVERIFY(QTest::qWaitFor([&] { return ptz_device->childCount() == 1; }, 2000));
            ptz_tree->setCurrentItem(ptz_device->child(0));
            ptz_button->click();
            QVERIFY(QTest::qWaitFor([&] { return ptz_api.queries.size() == 1; }, 2000));
            ptz_reply = pending_ptz(*ptz_window);
            ptz_peer = ptz_api.queries[0].socket;
            QVERIFY(ptz_reply && ptz_peer);
            QCOMPARE(ptz_reply->url().port(), int(ptz_api.server.serverPort()));
            QCOMPARE(window.findChild<QLineEdit*>(QStringLiteral("platform_address"))->text(), original_address);
            // 预置位调用复用同一超时窗口，仍不增加串行10秒等待。
            QVERIFY(preset_control_api.start());
            preset_control_window = std::make_unique<monitor_window>();
            QVERIFY(select_gb_channel(*preset_control_window));
            QVERIFY(
                load_preset_rows(*preset_control_window, preset_control_api, {QJsonObject{{"preset_id", "1"}, {"preset_name", "timeout-target"}}}));
            auto* control_list = preset_control_window->findChild<QTreeWidget*>(QStringLiteral("preset_list"));
            control_list->setCurrentItem(control_list->topLevelItem(0));
            preset_control_button = preset_control_window->findChild<QPushButton*>(QStringLiteral("preset_goto"));
            preset_control_result = preset_control_window->findChild<QLabel*>(QStringLiteral("preset_result"));
            QVERIFY(preset_control_button && preset_control_result);
            preset_control_button->click();
            QVERIFY(QTest::qWaitFor([&] { return preset_control_api.queries.size() == 2; }, 2000));
            preset_control_reply = pending_ptz(*preset_control_window);
            preset_control_peer = preset_control_api.queries[1].socket;
            QVERIFY(preset_control_reply && preset_control_peer);
            QCOMPARE(preset_control_reply->url().port(), int(preset_control_api.server.serverPort()));
            QCOMPARE(window.findChild<QLineEdit*>(QStringLiteral("platform_address"))->text(), original_address);
            QSettings().setValue(QStringLiteral("platform_url"), original_address);
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
        if (preset_window)
        {
            QTRY_VERIFY_WITH_TIMEOUT(preset_button->isEnabled(), 2000);
            QVERIFY(preset_result->text().contains(QStringLiteral("预置位")) && preset_result->text().contains(QStringLiteral("timed out")));
            QVERIFY(preset_window->findChild<QPushButton*>(QStringLiteral("device_info_query"))->isEnabled());
            QVERIFY(preset_window->findChild<QPushButton*>(QStringLiteral("device_status_query"))->isEnabled());
            QCOMPARE(preset_window->findChild<QTreeWidget*>(QStringLiteral("preset_list"))->topLevelItemCount(), 0);
            QTRY_VERIFY_WITH_TIMEOUT(preset_reply.isNull() && preset_peer.isNull(), 1000);
            preset_window.reset();
            QVERIFY(preset_button.isNull() && preset_result.isNull());
        }
        if (ptz_window)
        {
            QVERIFY(QTest::qWaitFor([&] { return ptz_button->isEnabled(); }, 2000));
            QVERIFY(ptz_result->text().contains(QStringLiteral("timed out")) && ptz_result->text().contains(QStringLiteral("未确认")));
            QVERIFY(ptz_window->findChild<QPushButton*>(QStringLiteral("ptz_stop"))->isEnabled());
            QVERIFY(ptz_window->findChild<QPushButton*>(QStringLiteral("device_info_query"))->isEnabled());
            QVERIFY(ptz_window->findChild<QPushButton*>(QStringLiteral("preset_query"))->isEnabled());
            QVERIFY(QTest::qWaitFor([&] { return ptz_reply.isNull() && ptz_peer.isNull(); }, 1000));
            ptz_window.reset();
            QVERIFY(ptz_button.isNull() && ptz_result.isNull());
        }
        if (preset_control_window)
        {
            QVERIFY(QTest::qWaitFor([&] { return preset_control_button->isEnabled(); }, 2000));
            QVERIFY(preset_control_result->text().contains(QStringLiteral("timed out")) &&
                    preset_control_result->text().contains(QStringLiteral("未确认")));
            QVERIFY(preset_control_window->findChild<QPushButton*>(QStringLiteral("ptz_stop"))->isEnabled());
            QVERIFY(preset_control_window->findChild<QPushButton*>(QStringLiteral("preset_set"))->isEnabled());
            QVERIFY(QTest::qWaitFor([&] { return preset_control_reply.isNull() && preset_control_peer.isNull(); }, 1000));
            QCOMPARE(preset_control_api.queries.size(), 2);
            preset_control_window.reset();
            QVERIFY(preset_control_button.isNull() && preset_control_result.isNull());
        }
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
