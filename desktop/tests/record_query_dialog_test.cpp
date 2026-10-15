#include <memory>

#include <QTest>
#include <QLabel>
#include <QPointer>
#include <QVariant>
#include <QSpinBox>
#include <QComboBox>
#include <QDateTime>
#include <QSignalSpy>
#include <QJsonArray>
#include <QTcpServer>
#include <QTcpSocket>
#include <QStringList>
#include <QJsonObject>
#include <QPushButton>
#include <QTreeWidget>
#include <QHostAddress>
#include <QJsonDocument>
#include <QDateTimeEdit>
#include <QNetworkReply>
#include <QElapsedTimer>

#include "desktop/platform_client.h"
#include "desktop/record_query_dialog.h"

using namespace media_server;

namespace
{

const QString device = QStringLiteral("34020000001320000001");
const QString channel = QStringLiteral("34020000001310000001");
const QStringList fields{QStringLiteral("device_id"),
                         QStringLiteral("name"),
                         QStringLiteral("start_time"),
                         QStringLiteral("end_time"),
                         QStringLiteral("type"),
                         QStringLiteral("file_path"),
                         QStringLiteral("address"),
                         QStringLiteral("secrecy"),
                         QStringLiteral("recorder_id"),
                         QStringLiteral("file_size")};

void respond(QTcpSocket* socket, int status, const QByteArray& body)
{
    if (!socket || socket->state() != QAbstractSocket::ConnectedState)
    {
        return;
    }
    socket->write("HTTP/1.1 " + QByteArray::number(status) + " Test\r\nContent-Type: application/json\r\nContent-Length: " +
                  QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
    socket->disconnectFromHost();
}

// 使用真实随机TCP端口；服务端保留完整请求，按用例显式交付响应。
class record_fixture final : public QObject
{
   public:
    struct pending
    {
        QPointer<QTcpSocket> socket;
        QByteArray request_line;
        QJsonObject body;
    };

    record_fixture()
    {
        connect(&server,
                &QTcpServer::newConnection,
                this,
                [this]()
                {
                    while (server.hasPendingConnections())
                    {
                        auto* socket = server.nextPendingConnection();
                        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                        connect(socket,
                                &QTcpSocket::readyRead,
                                this,
                                [this, socket]()
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
                                    requests.append(
                                        {socket, bytes.left(bytes.indexOf("\r\n")), QJsonDocument::fromJson(bytes.mid(end + 4, length)).object()});
                                });
                    }
                });
    }

    bool start(platform_client& client)
    {
        return server.listen(QHostAddress::LocalHost, 0) && client.set_base_url(QUrl(QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort())));
    }

    QTcpServer server;
    QList<pending> requests;
};

QJsonObject row(const QString& name = QStringLiteral("录像一"))
{
    return {{"device_id", channel},
            {"name", name},
            {"start_time", "2026-10-15T01:02:03"},
            {"end_time", "2026-10-15T02:03:04"},
            {"type", "time"},
            {"file_path", "/camera/录像<&>.ps"},
            {"address", "现场<&>"},
            {"secrecy", "0"},
            {"recorder_id", "0007"},
            {"file_size", "001234"}};
}

QJsonObject response(const QJsonArray& records)
{
    return {{"device_id", device}, {"channel_id", channel}, {"command", "RecordInfo"}, {"sn", 27}, {"sum_num", records.size()}, {"records", records}};
}

QPointer<QNetworkReply> pending_reply(platform_client& client)
{
    for (auto* reply : client.findChildren<QNetworkReply*>())
    {
        if (!reply->isFinished())
        {
            return reply;
        }
    }
    return {};
}

struct controls
{
    explicit controls(record_query_dialog& dialog)
        : start(dialog.findChild<QDateTimeEdit*>(QStringLiteral("record_start"))),
          end(dialog.findChild<QDateTimeEdit*>(QStringLiteral("record_end"))),
          type(dialog.findChild<QComboBox*>(QStringLiteral("record_type"))),
          query(dialog.findChild<QPushButton*>(QStringLiteral("record_query"))),
          cancel(dialog.findChild<QPushButton*>(QStringLiteral("record_cancel"))),
          offset(dialog.findChild<QSpinBox*>(QStringLiteral("record_utc_offset"))),
          play(dialog.findChild<QPushButton*>(QStringLiteral("record_play"))),
          list(dialog.findChild<QTreeWidget*>(QStringLiteral("record_list"))),
          result(dialog.findChild<QLabel*>(QStringLiteral("record_result")))
    {
    }

    bool valid() const { return start && end && type && query && cancel && offset && play && list && result; }
    bool idle() const { return start->isEnabled() && end->isEnabled() && type->isEnabled() && query->isEnabled() && !cancel->isEnabled(); }
    bool busy() const { return !start->isEnabled() && !end->isEnabled() && !type->isEnabled() && !query->isEnabled() && cancel->isEnabled(); }

    QDateTimeEdit* start;
    QDateTimeEdit* end;
    QComboBox* type;
    QPushButton* query;
    QPushButton* cancel;
    QSpinBox* offset;
    QPushButton* play;
    QTreeWidget* list;
    QLabel* result;
};

}    // namespace

class record_query_dialog_test final : public QObject
{
    Q_OBJECT

   private slots:
    void playback_utc_offset_data()
    {
        QTest::addColumn<int>("offset_minutes");
        for (const int offset : {0, 480, 345, -210, 840, -840})
        {
            QTest::newRow(qPrintable(QString::number(offset))) << offset;
        }
    }

    void playback_utc_offset()
    {
        QFETCH(int, offset_minutes);
        platform_client client;
        record_fixture api;
        QVERIFY(api.start(client));
        record_query_dialog dialog(client, device, channel);
        controls ui(dialog);
        QVERIFY(ui.valid());
        QCOMPARE(ui.offset->minimum(), -840);
        QCOMPARE(ui.offset->maximum(), 840);
        ui.offset->setValue(offset_minutes);
        QSignalSpy requested(&dialog, &record_query_dialog::playback_requested);
        QVERIFY(requested.isValid());
        ui.query->click();
        QTRY_COMPARE_WITH_TIMEOUT(api.requests.size(), qsizetype(1), 1000);
        auto first = row(QStringLiteral("未选择的第一段"));
        first.insert("start_time", "2026-10-15T03:04:05");
        first.insert("end_time", "2026-10-15T04:05:06");
        auto selected = row(QStringLiteral("所选第二段"));
        selected.insert("file_path", "https://untrusted.invalid/not-a-playback.ps");
        respond(api.requests[0].socket, 200, QJsonDocument(response({first, selected})).toJson());
        QTRY_COMPARE_WITH_TIMEOUT(ui.list->topLevelItemCount(), 2, 1000);
        ui.list->setCurrentItem(ui.list->topLevelItem(1));
        QVERIFY(ui.play->isEnabled());
        ui.play->click();
        QCOMPARE(requested.size(), qsizetype(1));
        const auto start_utc = QDateTime(QDate(2026, 10, 15), QTime(1, 2, 3), Qt::UTC).toSecsSinceEpoch();
        const auto end_utc = QDateTime(QDate(2026, 10, 15), QTime(2, 3, 4), Qt::UTC).toSecsSinceEpoch();
        QCOMPARE(requested[0][0].toLongLong(), start_utc - offset_minutes * 60);
        QCOMPARE(requested[0][1].toLongLong(), end_utc - offset_minutes * 60);
        // 只发所选目录项的规范时间；目录文件路径不构成媒体请求。
        QCOMPARE(api.requests.size(), qsizetype(1));
        QVERIFY(ui.result->text().contains(QStringLiteral("UTC 偏移 %1 分钟").arg(offset_minutes)));
    }

    void playback_invalid_record_range_data()
    {
        QTest::addColumn<QString>("first");
        QTest::addColumn<QString>("last");
        const QString valid_first = QStringLiteral("2026-10-15T01:02:03");
        const QString valid_last = QStringLiteral("2026-10-15T02:03:04");
        QTest::newRow("missing-start") << QString() << valid_last;
        QTest::newRow("missing-end") << valid_first << QString();
        QTest::newRow("invalid-day") << QStringLiteral("2026-02-30T01:02:03") << valid_last;
        QTest::newRow("invalid-clock") << QStringLiteral("2026-10-15T25:02:03") << valid_last;
        QTest::newRow("not-canonical-space") << QStringLiteral("2026-10-15 01:02:03") << valid_last;
        QTest::newRow("utc-suffix") << (valid_first + "Z") << valid_last;
        QTest::newRow("fractional-second") << (valid_first + ".000") << valid_last;
        QTest::newRow("equal") << valid_first << valid_first;
        QTest::newRow("reversed") << valid_last << valid_first;
        QTest::newRow("epoch-zero") << QStringLiteral("1970-01-01T00:00:00") << QStringLiteral("1970-01-01T00:00:01");
        QTest::newRow("negative-epoch") << QStringLiteral("1969-12-31T23:59:58") << QStringLiteral("1969-12-31T23:59:59");
    }

    void playback_invalid_record_range()
    {
        QFETCH(QString, first);
        QFETCH(QString, last);
        platform_client client;
        record_fixture api;
        QVERIFY(api.start(client));
        record_query_dialog dialog(client, device, channel);
        controls ui(dialog);
        QVERIFY(ui.valid());
        ui.offset->setValue(0);
        QSignalSpy requested(&dialog, &record_query_dialog::playback_requested);
        QVERIFY(requested.isValid());
        ui.query->click();
        QTRY_COMPARE_WITH_TIMEOUT(api.requests.size(), qsizetype(1), 1000);
        auto invalid = row();
        invalid.insert("start_time", first);
        invalid.insert("end_time", last);
        respond(api.requests[0].socket, 200, QJsonDocument(response({invalid})).toJson());
        QTRY_COMPARE_WITH_TIMEOUT(ui.list->topLevelItemCount(), 1, 1000);
        ui.list->setCurrentItem(ui.list->topLevelItem(0));
        QVERIFY(ui.play->isEnabled());
        ui.play->click();
        QCOMPARE(requested.size(), qsizetype(0));
        QVERIFY(ui.result->text().contains(QStringLiteral("缺少合法时间范围")));
        QCOMPARE(api.requests.size(), qsizetype(1));
    }

    void playback_requires_selected_record()
    {
        platform_client client;
        record_fixture api;
        QVERIFY(api.start(client));
        record_query_dialog dialog(client, device, channel);
        controls ui(dialog);
        QVERIFY(ui.valid());
        QSignalSpy requested(&dialog, &record_query_dialog::playback_requested);
        QVERIFY(requested.isValid());
        QVERIFY(!ui.play->isEnabled());
        ui.play->click();
        QCOMPARE(requested.size(), qsizetype(0));
        ui.query->click();
        QTRY_COMPARE_WITH_TIMEOUT(api.requests.size(), qsizetype(1), 1000);
        respond(api.requests[0].socket, 200, QJsonDocument(response({row()})).toJson());
        QTRY_COMPARE_WITH_TIMEOUT(ui.list->topLevelItemCount(), 1, 1000);
        auto* item = ui.list->topLevelItem(0);
        ui.list->setCurrentItem(item);
        QVERIFY(item->isSelected());
        QVERIFY(ui.play->isEnabled());
        ui.list->clearSelection();
        QCOMPARE(ui.list->currentItem(), item);
        QVERIFY(ui.list->selectedItems().isEmpty());
        // 清除选择不会清除焦点项；仅 currentItem 非空不能授权回放。
        ui.play->click();
        QCOMPARE(requested.size(), qsizetype(0));
        QVERIFY(!ui.play->isEnabled());
        QCOMPARE(api.requests.size(), qsizetype(1));
    }

    void playback_query_pending_disables_play()
    {
        platform_client client;
        record_fixture api;
        QVERIFY(api.start(client));
        record_query_dialog dialog(client, device, channel);
        controls ui(dialog);
        QVERIFY(ui.valid());
        QSignalSpy requested(&dialog, &record_query_dialog::playback_requested);
        QVERIFY(requested.isValid());
        ui.query->click();
        QTRY_COMPARE_WITH_TIMEOUT(api.requests.size(), qsizetype(1), 1000);
        respond(api.requests[0].socket, 200, QJsonDocument(response({row()})).toJson());
        QTRY_COMPARE_WITH_TIMEOUT(ui.list->topLevelItemCount(), 1, 1000);
        ui.list->setCurrentItem(ui.list->topLevelItem(0));
        QVERIFY(ui.play->isEnabled());
        ui.query->click();
        QVERIFY(ui.busy());
        QVERIFY(!ui.play->isEnabled());
        QCOMPARE(ui.list->topLevelItemCount(), 0);
        ui.play->click();
        QCOMPARE(requested.size(), qsizetype(0));
        QTRY_COMPARE_WITH_TIMEOUT(api.requests.size(), qsizetype(2), 1000);
        respond(api.requests[1].socket, 200, QJsonDocument(response({row(QStringLiteral("new"))})).toJson());
        QTRY_COMPARE_WITH_TIMEOUT(ui.list->topLevelItemCount(), 1, 1000);
        QVERIFY(!ui.play->isEnabled());
        ui.list->setCurrentItem(ui.list->topLevelItem(0));
        QVERIFY(ui.play->isEnabled());
        ui.play->click();
        QCOMPARE(requested.size(), qsizetype(1));
        QCOMPARE(api.requests.size(), qsizetype(2));
    }

    void successful_directories_data()
    {
        QTest::addColumn<QJsonArray>("records");
        QTest::newRow("single") << QJsonArray{row()};
        QTest::newRow("multiple") << QJsonArray{row(QStringLiteral("第一段")), row(QStringLiteral("第二段"))};
        QTest::newRow("explicit-zero") << QJsonArray{};
        QTest::newRow("duplicate-preserved") << QJsonArray{row(), row()};
        QTest::newRow("optional-missing") << QJsonArray{QJsonObject{{"device_id", channel}, {"name", "minimal"}}};
        QTest::newRow("empty-name-is-present") << QJsonArray{QJsonObject{{"device_id", channel}, {"name", ""}}};
        QTest::newRow("chinese-literal") << QJsonArray{row(QStringLiteral("中文<&>\"'\\换行\n录像"))};
        QJsonArray maximum;
        for (int index = 0; index != 1000; ++index)
        {
            maximum.append(row(QString::number(index)));
        }
        QTest::newRow("maximum-1000") << maximum;
    }

    void successful_directories()
    {
        QFETCH(QJsonArray, records);
        platform_client client;
        record_fixture api;
        QVERIFY(api.start(client));
        record_query_dialog dialog(client, device, channel);
        controls ui(dialog);
        QVERIFY(ui.valid());
        QCOMPARE(dialog.windowModality(), Qt::NonModal);
        QCOMPARE(ui.list->columnCount(), 10);
        QCOMPARE(ui.result->textFormat(), Qt::PlainText);
        QVERIFY(ui.idle());
        ui.query->click();
        QVERIFY(ui.busy());
        QVERIFY(QTest::qWaitFor([&] { return api.requests.size() == 1; }, 1000));
        const auto reply = pending_reply(client);
        QVERIFY(reply);
        respond(api.requests[0].socket, 200, QJsonDocument(response(records)).toJson());
        QVERIFY(QTest::qWaitFor([&] { return ui.idle(); }, 1000));
        QCOMPARE(ui.list->topLevelItemCount(), records.size());
        for (int index = 0; index < records.size(); ++index)
        {
            for (int field = 0; field < fields.size(); ++field)
            {
                QCOMPARE(ui.list->topLevelItem(index)->text(field), records[index].toObject().value(fields[field]).toString());
            }
        }
        QVERIFY(ui.result->text().contains(QStringLiteral("设备声明 %1 条").arg(records.size())));
        QVERIFY(ui.result->text().contains(QStringLiteral("已收到 %1 条").arg(records.size())));
        QVERIFY(QTest::qWaitFor([&] { return reply.isNull(); }, 1000));
    }

    void request_parameters_data()
    {
        QTest::addColumn<QString>("type");
        for (const auto& type : {"all", "time", "alarm", "manual"})
        {
            QTest::newRow(type) << QString::fromLatin1(type);
        }
    }

    void request_parameters()
    {
        QFETCH(QString, type);
        platform_client client;
        record_fixture api;
        QVERIFY(api.start(client));
        record_query_dialog dialog(client, device, channel);
        controls ui(dialog);
        QVERIFY(ui.valid());
        ui.start->setDate(QDate(2026, 10, 15));
        ui.start->setTime(QTime(1, 2, 3));
        ui.end->setDate(QDate(2026, 10, 15));
        ui.end->setTime(QTime(2, 3, 4));
        ui.type->setCurrentIndex(ui.type->findData(type));
        new QTreeWidgetItem(ui.list, {QStringLiteral("old-row")});
        ui.query->click();
        QCOMPARE(ui.list->topLevelItemCount(), 0);
        QVERIFY(ui.busy());
        ui.query->click();
        QVERIFY(QTest::qWaitFor([&] { return api.requests.size() == 1; }, 1000));
        QCOMPARE(api.requests[0].request_line,
                 QByteArray("POST /api/devices/34020000001320000001/channels/34020000001310000001/records/query HTTP/1.1"));
        QCOMPARE(api.requests[0].body, QJsonObject({{"start_time", "2026-10-15T01:02:03"}, {"end_time", "2026-10-15T02:03:04"}, {"type", type}}));
        respond(api.requests[0].socket, 200, QJsonDocument(response({})).toJson());
        QVERIFY(QTest::qWaitFor([&] { return ui.idle(); }, 1000));
        QCOMPARE(api.requests.size(), 1);
    }

    void invalid_parameters_data()
    {
        QTest::addColumn<QString>("invalid");
        QTest::newRow("equal") << QStringLiteral("equal");
        QTest::newRow("reversed") << QStringLiteral("reversed");
        QTest::newRow("invalid-type") << QStringLiteral("type");
    }

    void invalid_parameters()
    {
        QFETCH(QString, invalid);
        platform_client client;
        record_fixture api;
        QVERIFY(api.start(client));
        record_query_dialog dialog(client, device, channel);
        controls ui(dialog);
        QVERIFY(ui.valid());
        if (invalid == QStringLiteral("type"))
        {
            ui.type->addItem(QStringLiteral("非法"), QStringLiteral("other"));
            ui.type->setCurrentIndex(ui.type->count() - 1);
        }
        else
        {
            ui.start->setDateTime(ui.end->dateTime().addSecs(invalid == QStringLiteral("equal") ? 0 : 1));
        }
        ui.query->click();
        QVERIFY(ui.idle());
        QVERIFY(ui.result->text().contains(QStringLiteral("参数无效")));
        QTest::qWait(30);
        QCOMPARE(api.requests.size(), 0);
        QVERIFY(!pending_reply(client));
    }

    void rejected_responses_data()
    {
        QTest::addColumn<int>("status");
        QTest::addColumn<QByteArray>("body");
        const auto valid = response({row()});
        const auto add = [](const QByteArray& label, const QJsonObject& object, int status = 200)
        { QTest::newRow(label.constData()) << status << QJsonDocument(object).toJson(); };
        for (const auto& key : {"device_id", "channel_id", "command", "sn", "sum_num", "records"})
        {
            auto missing = valid;
            missing.remove(QString::fromLatin1(key));
            add(QByteArray("missing-") + key, missing);
        }
        for (const auto& key : {"device_id", "channel_id", "command"})
        {
            auto wrong = valid;
            wrong.insert(QString::fromLatin1(key), "wrong");
            add(QByteArray("wrong-") + key, wrong);
        }
        const QJsonArray invalid_sn{0, -1, 1.5, "27", QJsonValue::Null};
        for (int index = 0; index < invalid_sn.size(); ++index)
        {
            auto wrong = valid;
            wrong.insert("sn", invalid_sn[index]);
            add(QByteArray("invalid-sn-") + QByteArray::number(index), wrong);
        }
        const QJsonArray invalid_totals{-1, 0, 1.5, 1001, "1", QJsonValue::Null};
        for (int index = 0; index < invalid_totals.size(); ++index)
        {
            auto wrong = valid;
            wrong.insert("sum_num", invalid_totals[index]);
            add(QByteArray("invalid-total-") + QByteArray::number(index), wrong);
        }
        auto object_records = valid;
        object_records.insert("records", QJsonObject{});
        add("records-object", object_records);
        auto nonobject = valid;
        nonobject.insert("records", QJsonArray{QStringLiteral("invalid")});
        add("record-not-object", nonobject);
        for (const auto& key : {"device_id", "name"})
        {
            auto missing = row();
            missing.remove(QString::fromLatin1(key));
            add(QByteArray("record-missing-") + key, response({missing}));
        }
        auto wrong_device = row();
        wrong_device.insert("device_id", device);
        add("record-device-is-parent", response({wrong_device}));
        for (const auto& key : fields)
        {
            for (const auto& value : QJsonArray{42, true, QJsonValue::Null, QJsonObject{}, QJsonArray{}})
            {
                auto invalid = row();
                invalid.insert(key, value);
                add(QByteArray("record-field-") + key.toUtf8() + '-' + QByteArray::number(value.type()), response({row(), invalid}));
            }
        }
        add("http-201", valid, 201);
        add("http-202", valid, 202);
        add("http-422", QJsonObject{{"error", "record_result_too_large"}}, 422);
        add("http-502", QJsonObject{{"error", "record_query_failed"}}, 502);
        QTest::newRow("invalid-json") << 200 << QByteArray("{invalid");
        QTest::newRow("json-array") << 200 << QByteArray("[]");
    }

    void rejected_responses()
    {
        QFETCH(int, status);
        QFETCH(QByteArray, body);
        platform_client client;
        record_fixture api;
        QVERIFY(api.start(client));
        record_query_dialog dialog(client, device, channel);
        controls ui(dialog);
        QVERIFY(ui.valid());
        ui.query->click();
        QVERIFY(QTest::qWaitFor([&] { return api.requests.size() == 1; }, 1000));
        respond(api.requests[0].socket, status, body);
        QVERIFY(QTest::qWaitFor([&] { return ui.idle(); }, 1000));
        QCOMPARE(ui.list->topLevelItemCount(), 0);
        QVERIFY(!ui.result->text().contains(QStringLiteral("已收到")));
        if (status == 422)
        {
            QVERIFY(ui.result->text().contains(QStringLiteral("缩小查询时间范围")));
        }
        else
        {
            QVERIFY2(ui.result->text().contains(QStringLiteral("失败")), qPrintable(ui.result->text()));
        }
        // 失败后允许用户明确重试，并且不沿用被拒绝的行。
        ui.query->click();
        QVERIFY(QTest::qWaitFor([&] { return api.requests.size() == 2; }, 1000));
        respond(api.requests[1].socket, 200, QJsonDocument(response({row(QStringLiteral("fresh"))})).toJson());
        QVERIFY(QTest::qWaitFor([&] { return ui.idle(); }, 1000));
        QCOMPARE(ui.list->topLevelItemCount(), 1);
        QCOMPARE(ui.list->topLevelItem(0)->text(1), QStringLiteral("fresh"));
    }

    void cancel_then_new_query_data()
    {
        QTest::addColumn<int>("old_status");
        QTest::newRow("old-success") << 200;
        QTest::newRow("old-error") << 502;
    }

    void cancel_aborts_hanging_reply()
    {
        platform_client client;
        record_fixture api;
        QVERIFY(api.start(client));
        record_query_dialog dialog(client, device, channel);
        controls ui(dialog);
        QVERIFY(ui.valid());
        ui.query->click();
        QVERIFY(QTest::qWaitFor([&] { return api.requests.size() == 1; }, 1000));
        const auto reply = pending_reply(client);
        const auto peer = api.requests[0].socket;
        QVERIFY(reply && peer);
        ui.cancel->click();
        QVERIFY(ui.idle());
        QVERIFY(reply && reply->isFinished());
        QCOMPARE(reply->error(), QNetworkReply::OperationCanceledError);
        QVERIFY(ui.result->text().contains(QStringLiteral("已取消")));
        QVERIFY(QTest::qWaitFor([&] { return reply.isNull() && peer.isNull(); }, 1000));
        QCOMPARE(api.requests.size(), 1);
    }

    void cancel_then_new_query()
    {
        QFETCH(int, old_status);
        platform_client client;
        record_fixture api;
        QVERIFY(api.start(client));
        record_query_dialog dialog(client, device, channel);
        controls ui(dialog);
        QVERIFY(ui.valid());
        ui.query->click();
        QVERIFY(QTest::qWaitFor([&] { return api.requests.size() == 1; }, 1000));
        const auto old = pending_reply(client);
        const auto peer = api.requests[0].socket;
        QVERIFY(old && peer);
        // 响应已写入TCP但GUI尚未交付完成：取消必须先移除回调context。
        respond(peer,
                old_status,
                old_status == 200 ? QJsonDocument(response({row(QStringLiteral("stale"))})).toJson() : QByteArray(R"({"error":"stale-error"})"));
        ui.cancel->click();
        QVERIFY(ui.idle());
        QVERIFY(ui.result->text().contains(QStringLiteral("已取消")));
        ui.query->click();
        QVERIFY(ui.busy());
        QVERIFY(QTest::qWaitFor([&] { return api.requests.size() == 2; }, 1000));
        QVERIFY(ui.busy());
        QCOMPARE(ui.list->topLevelItemCount(), 0);
        QVERIFY(!ui.result->text().contains(QStringLiteral("stale")));
        respond(api.requests[1].socket, 200, QJsonDocument(response({row(QStringLiteral("new-result"))})).toJson());
        QVERIFY(QTest::qWaitFor([&] { return ui.idle(); }, 1000));
        QCOMPARE(ui.list->topLevelItemCount(), 1);
        QCOMPARE(ui.list->topLevelItem(0)->text(1), QStringLiteral("new-result"));
        QVERIFY(QTest::qWaitFor([&] { return old.isNull() && peer.isNull(); }, 1000));
    }

    void close_pending_data()
    {
        QTest::addColumn<QString>("action");
        QTest::addColumn<int>("old_status");
        for (const auto& action : {"close", "reject", "destruct", "parent-destruct"})
        {
            for (int status : {0, 200, 502})
            {
                QTest::newRow((QByteArray(action) + '-' + QByteArray::number(status)).constData()) << QString::fromLatin1(action) << status;
            }
        }
    }

    void close_pending()
    {
        QFETCH(QString, action);
        QFETCH(int, old_status);
        platform_client client;
        record_fixture api;
        QVERIFY(api.start(client));
        auto parent = std::make_unique<QWidget>();
        QPointer<record_query_dialog> dialog = new record_query_dialog(client, device, channel, parent.get());
        controls ui(*dialog);
        QVERIFY(ui.valid());
        dialog->show();
        ui.query->click();
        QVERIFY(QTest::qWaitFor([&] { return api.requests.size() == 1; }, 1000));
        const auto reply = pending_reply(client);
        const auto peer = api.requests[0].socket;
        QVERIFY(reply && peer);
        if (old_status != 0)
        {
            respond(peer,
                    old_status,
                    old_status == 200 ? QJsonDocument(response({row(QStringLiteral("stale-close"))})).toJson()
                                      : QByteArray(R"({"error":"stale-close"})"));
        }
        if (action == QStringLiteral("close"))
        {
            dialog->close();
        }
        else if (action == QStringLiteral("reject"))
        {
            dialog->reject();
        }
        else if (action == QStringLiteral("destruct"))
        {
            delete dialog.data();
        }
        else
        {
            parent.reset();
        }
        QVERIFY(QTest::qWaitFor([&] { return reply.isNull() && peer.isNull(); }, 1000));
        if (dialog)
        {
            QVERIFY(!dialog->isVisible());
            QCOMPARE(ui.list->topLevelItemCount(), 0);
            QVERIFY(!ui.result->text().contains(QStringLiteral("stale-close")));
        }
        else
        {
            QVERIFY(action == QStringLiteral("destruct") || action == QStringLiteral("parent-destruct"));
        }
    }

    void real_twenty_second_timeout()
    {
        platform_client client;
        record_fixture api;
        QVERIFY(api.start(client));
        record_query_dialog dialog(client, device, channel);
        controls ui(dialog);
        QVERIFY(ui.valid());
        QElapsedTimer elapsed;
        elapsed.start();
        ui.query->click();
        QVERIFY(QTest::qWaitFor([&] { return api.requests.size() == 1; }, 1000));
        const auto reply = pending_reply(client);
        const auto peer = api.requests[0].socket;
        QVERIFY(reply && peer);
        QVERIFY(QTest::qWaitFor([&] { return ui.idle(); }, 24'000));
        QVERIFY2(elapsed.elapsed() >= 19'000, qPrintable(QStringLiteral("timeout ended at %1ms").arg(elapsed.elapsed())));
        QVERIFY(ui.result->text().contains(QStringLiteral("timed out")));
        QCOMPARE(ui.list->topLevelItemCount(), 0);
        QVERIFY(QTest::qWaitFor([&] { return reply.isNull() && peer.isNull(); }, 1000));
        QCOMPARE(api.requests.size(), 1);
    }
};

QTEST_MAIN(record_query_dialog_test)
#include "record_query_dialog_test.moc"
