#include <memory>

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
#include <QElapsedTimer>
#include <QCoreApplication>
#include <QCryptographicHash>

#include "desktop/decode_thread.h"
#include "desktop/playback_view.h"
#include "desktop/platform_client.h"

namespace media_server
{

namespace
{

const QByteArray create_line("POST /api/devices/34020000001320000001/channels/34020000001320000002/playback HTTP/1.1");
const QString first_id = QStringLiteral("11111111-1111-4111-8111-111111111111");

play_target historical_target()
{
    return {
        play_target::kind::gb_channel, QStringLiteral("34020000001320000001"), QStringLiteral("34020000001320000002"), QStringLiteral("有限录像")};
}

// 每条 TCP 连接解析完整 HTTP 请求后才进入屏障；不靠固定等待猜测请求已到达。
class http_fixture final : public QObject
{
   public:
    struct pending
    {
        QPointer<QTcpSocket> socket;
        QByteArray line;
        QByteArray body;
    };

    http_fixture()
    {
        connect(&server_,
                &QTcpServer::newConnection,
                this,
                [this]()
                {
                    while (server_.hasPendingConnections())
                    {
                        auto* socket = server_.nextPendingConnection();
                        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                        connect(socket,
                                &QTcpSocket::readyRead,
                                this,
                                [this, socket]()
                                {
                                    auto bytes = socket->property("request").toByteArray();
                                    bytes += socket->readAll();
                                    socket->setProperty("request", bytes);
                                    const auto end = bytes.indexOf("\r\n\r\n");
                                    if (end < 0 || socket->property("parsed").toBool())
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
                                                parse_error = true;
                                                return;
                                            }
                                        }
                                    }
                                    if (bytes.size() < end + 4 + length)
                                    {
                                        return;
                                    }
                                    if (bytes.size() != end + 4 + length)
                                    {
                                        parse_error = true;
                                        return;
                                    }
                                    socket->setProperty("parsed", true);
                                    requests.append({socket, bytes.first(bytes.indexOf("\r\n")), bytes.mid(end + 4, length)});
                                });
                    }
                });
    }

    bool start() { return server_.listen(QHostAddress::LocalHost, 0); }
    QUrl url() const { return QUrl(QStringLiteral("http://127.0.0.1:%1").arg(server_.serverPort())); }

    bool respond(qsizetype index, int status, const QJsonObject& object = {})
    {
        if (index >= requests.size() || !requests[index].socket)
        {
            return false;
        }
        const auto body = status == 204 ? QByteArray{} : QJsonDocument(object).toJson(QJsonDocument::Compact);
        const auto response = "HTTP/1.1 " + QByteArray::number(status) +
                              " Test\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(body.size()) +
                              "\r\nConnection: close\r\n\r\n" + body;
        auto* socket = requests[index].socket.data();
        const bool queued = socket->write(response) == response.size();
        socket->disconnectFromHost();
        return queued;
    }

    bool stream(qsizetype index, const QByteArray& payload)
    {
        if (index >= requests.size() || !requests[index].socket)
        {
            return false;
        }
        const auto response = QByteArray("HTTP/1.1 200 OK\r\nContent-Type: video/mpeg\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n") +
                              QByteArray::number(static_cast<qlonglong>(payload.size()), 16) + "\r\n" + payload + "\r\n0\r\n\r\n";
        auto* socket = requests[index].socket.data();
        const bool queued = socket->write(response) == response.size();
        socket->disconnectFromHost();
        return queued;
    }

    // 只写入原始字节、不关闭连接，用于在同一响应中分段发送。
    bool send(qsizetype index, const QByteArray& bytes)
    {
        return index < requests.size() && requests[index].socket && requests[index].socket->write(bytes) == bytes.size();
    }

    QList<pending> requests;
    bool parse_error{};

   private:
    QTcpServer server_;
};

QJsonObject ticket(const http_fixture& media, const QString& id = first_id)
{
    auto url = media.url();
    url.setPath(QStringLiteral("/play/gb28181/%1/fixture-token.ps").arg(id));
    return {{"playback_id", id}, {"ps_url", url.toString()}, {"expires_at", "2026-10-16T01:00:00Z"}, {"deadline", "2026-10-16T01:02:00Z"}};
}

QByteArray delete_line(const QString& id = first_id) { return "DELETE /api/playbacks/" + id.toUtf8() + " HTTP/1.1"; }

QByteArray control_line(const char* action) { return "POST /api/playbacks/" + first_id.toUtf8() + "/" + action + " HTTP/1.1"; }

QByteArray chunk(const QByteArray& payload) { return QByteArray::number(static_cast<qlonglong>(payload.size()), 16) + "\r\n" + payload + "\r\n"; }

const QByteArray stream_header("HTTP/1.1 200 OK\r\nContent-Type: video/mpeg\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n");

// platform_client 未声明 Q_OBJECT；不能用继承来的 QObject 元对象匹配其真实子类类型。
QList<QPointer<platform_client>> operation_clients(platform_client& client)
{
    QList<QPointer<platform_client>> result;
    for (auto* child : client.children())
    {
        if (auto* operation = dynamic_cast<platform_client*>(child))
        {
            result.append(operation);
        }
    }
    return result;
}

// Paint 前 image() 就是本轮实际绘制的图像；该事件内没有事件循环或图像替换。
class painted_frames final : public QObject
{
   public:
    explicit painted_frames(playback_view& view) : view_(view) { view.installEventFilter(this); }
    QList<QByteArray> hashes;
    QList<quint64> indices;

   protected:
    bool eventFilter(QObject* object, QEvent* event) override
    {
        if (object == &view_ && event->type() == QEvent::Paint && !view_.image().isNull() &&
            (indices.isEmpty() || indices.back() != view_.rendered_frames()))
        {
            const auto& image = view_.image();
            hashes.append(QCryptographicHash::hash(QByteArray(reinterpret_cast<const char*>(image.constBits()), image.sizeInBytes()),
                                                   QCryptographicHash::Sha256));
            indices.append(view_.rendered_frames());
        }
        return QObject::eventFilter(object, event);
    }

   private:
    playback_view& view_;
};

}    // namespace

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
        }
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(QCoreApplication::instance()->findChildren<decode_thread*>().isEmpty());
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
        QVERIFY(api.respond(0, 201, ticket(media)));
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
        QTest::qWait(200);
        QCOMPARE(view.rendered_frames(), frozen);
        QVERIFY(frozen < 50);
        QVERIFY(view.record_control_ready());
        view.set_record_paused(false);
        QTRY_COMPARE_WITH_TIMEOUT(api.requests.size(), qsizetype(3), 2000);
        QCOMPARE(api.requests[2].line, control_line("resume"));
        QVERIFY(api.respond(2, 204));
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
