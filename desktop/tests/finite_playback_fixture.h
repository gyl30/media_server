#ifndef DESKTOP_TESTS_FINITE_PLAYBACK_FIXTURE_H
#define DESKTOP_TESTS_FINITE_PLAYBACK_FIXTURE_H

#include <QUrl>
#include <QList>
#include <QEvent>
#include <QObject>
#include <QString>
#include <QPointer>
#include <QDateTime>
#include <QByteArray>
#include <QTcpServer>
#include <QTcpSocket>
#include <QJsonObject>
#include <QHostAddress>
#include <QJsonDocument>
#include <QCryptographicHash>

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
    const auto now = QDateTime::currentDateTimeUtc();
    return {{"playback_id", id},
            {"ps_url", url.toString()},
            {"expires_at", now.addSecs(60).toString(Qt::ISODateWithMs)},
            {"deadline", now.addSecs(120).toString(Qt::ISODateWithMs)}};
}

QByteArray delete_line(const QString& id = first_id) { return "DELETE /api/playbacks/" + id.toUtf8() + " HTTP/1.1"; }

QByteArray control_line(const char* action) { return "POST /api/playbacks/" + first_id.toUtf8() + "/" + action + " HTTP/1.1"; }

QByteArray chunk(const QByteArray& payload) { return QByteArray::number(static_cast<qlonglong>(payload.size()), 16) + "\r\n" + payload + "\r\n"; }

const QByteArray stream_header("HTTP/1.1 200 OK\r\nContent-Type: video/mpeg\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n");

// 保留真实 H.264/PCMA 负载，只平移 PES 的 PTS/DTS；SCR 不参与本测试的呈现时钟。
QByteArray shifted_pes_timestamps(QByteArray bytes, qint64 offset, bool shift_video = true, bool shift_audio = true)
{
    qsizetype position = 0;
    const QByteArray prefix("\0\0\1", 3);
    while ((position = bytes.indexOf(prefix, position)) >= 0)
    {
        if (position + 9 >= bytes.size())
        {
            break;
        }
        const auto id = static_cast<unsigned char>(bytes[position + 3]);
        if ((id >= 0xc0 && id <= 0xef) || id == 0xbd)
        {
            const auto flags = static_cast<unsigned char>(bytes[position + 7]) & 0xc0;
            if (position + 9 + (flags == 0xc0 ? 10 : flags == 0x80 ? 5 : 0) > bytes.size())
            {
                return {};
            }
            const bool shift = id >= 0xe0 ? shift_video : shift_audio;
            for (int index = 0; shift && index < (flags == 0xc0 ? 2 : flags == 0x80 ? 1 : 0); ++index)
            {
                auto* p = reinterpret_cast<unsigned char*>(bytes.data() + position + 9 + index * 5);
                const quint64 timestamp =
                    (quint64(p[0] & 0x0e) << 29) | (quint64(p[1]) << 22) | (quint64(p[2] & 0xfe) << 14) | (quint64(p[3]) << 7) | (p[4] >> 1);
                const quint64 shifted = timestamp + offset;
                p[0] = (p[0] & 0xf1) | ((shifted >> 29) & 0x0e);
                p[1] = (shifted >> 22) & 0xff;
                p[2] = ((shifted >> 14) & 0xfe) | 1;
                p[3] = (shifted >> 7) & 0xff;
                p[4] = ((shifted << 1) & 0xfe) | 1;
            }
            const auto length = (static_cast<unsigned char>(bytes[position + 4]) << 8) | static_cast<unsigned char>(bytes[position + 5]);
            position += length > 0 ? length + 6 : 4;
            continue;
        }
        position += 4;
    }
    return bytes;
}

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

}    // namespace media_server

#endif
