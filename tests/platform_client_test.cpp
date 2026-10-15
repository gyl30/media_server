#include <utility>

#include <QTest>
#include <QList>
#include <QTimer>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QHostAddress>

#include "desktop/platform_client.h"

namespace media_server
{
namespace
{

// 只替代 HTTP API 响应，不模拟媒体连接或解码。
void serve(QTcpServer& server, const QByteArray& response, QList<QByteArray>& requests, int delay_ms = 0)
{
    QObject::connect(&server,
                     &QTcpServer::newConnection,
                     &server,
                     [&server, response, &requests, delay_ms]()
                     {
                         while (server.hasPendingConnections())
                         {
                             auto* socket = server.nextPendingConnection();
                             QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                             QObject::connect(socket,
                                              &QTcpSocket::readyRead,
                                              socket,
                                              [socket, response, &requests, delay_ms, buffer = QByteArray{}, handled = false]() mutable
                                              {
                                                  buffer += socket->readAll();
                                                  const auto end = buffer.indexOf("\r\n\r\n");
                                                  if (handled || end < 0)
                                                  {
                                                      return;
                                                  }
                                                  int length = 0;
                                                  for (const auto& line : buffer.left(end).split('\n'))
                                                  {
                                                      if (line.toLower().startsWith("content-length:"))
                                                      {
                                                          length = line.mid(15).trimmed().toInt();
                                                      }
                                                  }
                                                  if (buffer.size() < end + 4 + length)
                                                  {
                                                      return;
                                                  }
                                                  handled = true;
                                                  requests.append(buffer);
                                                  if (delay_ms < 0)
                                                  {
                                                      return;
                                                  }
                                                  QTimer::singleShot(delay_ms,
                                                                     socket,
                                                                     [socket, response]()
                                                                     {
                                                                         socket->write(response);
                                                                         socket->disconnectFromHost();
                                                                     });
                                              });
                         }
                     });
}

QUrl base_url(const QTcpServer& server) { return QUrl(QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort())); }

QByteArray http_response(int status, const QByteArray& body)
{
    return "HTTP/1.1 " + QByteArray::number(status) +
           " Fixture\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" +
           body;
}

}    // namespace

class platform_client_test final : public QObject
{
    Q_OBJECT

   private slots:
    void valid_json_and_gateway_prefix()
    {
        QTcpServer server;
        QList<QByteArray> requests;
        serve(server, http_response(200, "{\"devices\":[]}"), requests);
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        platform_client client;
        auto url = base_url(server);
        url.setPath(QStringLiteral("/gateway/platform/"));
        QVERIFY(client.set_base_url(url));
        bool called = false;
        platform_response response;
        QPointer<QNetworkReply> reply = client.request("GET",
                                                       QStringLiteral("/api/devices"),
                                                       {},
                                                       this,
                                                       [&](platform_response value)
                                                       {
                                                           response = std::move(value);
                                                           called = true;
                                                       });
        QVERIFY(reply);
        QTRY_VERIFY_WITH_TIMEOUT(called, 1000);
        QVERIFY(response.ok());
        QCOMPARE(response.status, 200);
        QCOMPARE(response.network_error, QNetworkReply::NoError);
        QVERIFY(response.document.object().contains(QStringLiteral("devices")));
        QCOMPARE(requests.size(), 1);
        QVERIFY(requests.front().startsWith("GET /gateway/platform/api/devices HTTP/1.1\r\n"));
        QTRY_VERIFY_WITH_TIMEOUT(reply.isNull(), 1000);
    }

    void post_json()
    {
        QTcpServer server;
        QList<QByteArray> requests;
        serve(server, http_response(201, "{\"stream_id\":\"fixture\"}"), requests);
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        platform_client client;
        QVERIFY(client.set_base_url(base_url(server)));
        const QJsonObject body{{QStringLiteral("source"), QJsonObject{{QStringLiteral("source_id"), QStringLiteral("one")}}}};
        bool called = false;
        platform_response response;
        QVERIFY(client.request("POST",
                               QStringLiteral("/api/play"),
                               body,
                               this,
                               [&](platform_response value)
                               {
                                   response = std::move(value);
                                   called = true;
                               }));
        QTRY_VERIFY_WITH_TIMEOUT(called, 1000);
        QVERIFY(response.ok());
        QCOMPARE(response.status, 201);
        QCOMPARE(requests.size(), 1);
        QVERIFY(requests.front().startsWith("POST /api/play HTTP/1.1\r\n"));
        QCOMPARE(QJsonDocument::fromJson(requests.front().mid(requests.front().indexOf("\r\n\r\n") + 4)).object(), body);
    }

    void http_error()
    {
        QTcpServer server;
        QList<QByteArray> requests;
        serve(server, http_response(404, "{\"error\":\"missing\"}"), requests);
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        platform_client client;
        QVERIFY(client.set_base_url(base_url(server)));
        bool called = false;
        platform_response response;
        client.request("GET",
                       QStringLiteral("/api/sources"),
                       {},
                       this,
                       [&](platform_response value)
                       {
                           response = std::move(value);
                           called = true;
                       });
        QTRY_VERIFY_WITH_TIMEOUT(called, 1000);
        QVERIFY(!response.ok());
        QCOMPARE(response.status, 404);
        QCOMPARE(response.network_error, QNetworkReply::ContentNotFoundError);
        QCOMPARE(response.error, QStringLiteral("HTTP 404 · missing"));
    }

    void invalid_json()
    {
        QTcpServer server;
        QList<QByteArray> requests;
        serve(server, http_response(200, "[broken"), requests);
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        platform_client client;
        QVERIFY(client.set_base_url(base_url(server)));
        bool called = false;
        platform_response response;
        client.request("GET",
                       QStringLiteral("/api/devices"),
                       {},
                       this,
                       [&](platform_response value)
                       {
                           response = std::move(value);
                           called = true;
                       });
        QTRY_VERIFY_WITH_TIMEOUT(called, 1000);
        QVERIFY(!response.ok());
        QCOMPARE(response.status, 200);
        QCOMPARE(response.network_error, QNetworkReply::NoError);
        QCOMPARE(response.error, QStringLiteral("Invalid JSON response"));
    }

    void no_content()
    {
        QTcpServer server;
        QList<QByteArray> requests;
        serve(server, http_response(204, {}), requests);
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        platform_client client;
        QVERIFY(client.set_base_url(base_url(server)));
        bool called = false;
        platform_response response;
        client.request("DELETE",
                       QStringLiteral("/api/sources/one"),
                       {},
                       this,
                       [&](platform_response value)
                       {
                           response = std::move(value);
                           called = true;
                       });
        QTRY_VERIFY_WITH_TIMEOUT(called, 1000);
        QVERIFY(response.ok());
        QCOMPARE(response.status, 204);
        QVERIFY(response.document.isNull());
    }

    void timeout()
    {
        QTcpServer server;
        QList<QByteArray> requests;
        serve(server, {}, requests, -1);
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        platform_client client;
        QVERIFY(client.set_base_url(base_url(server)));
        bool called = false;
        platform_response response;
        QPointer<QNetworkReply> reply = client.request(
            "GET",
            QStringLiteral("/api/devices"),
            {},
            this,
            [&](platform_response value)
            {
                response = std::move(value);
                called = true;
            },
            40);
        QVERIFY(reply);
        QTRY_VERIFY_WITH_TIMEOUT(called, 1000);
        QVERIFY(!response.ok());
        QCOMPARE(response.network_error, QNetworkReply::TimeoutError);
        QTRY_VERIFY_WITH_TIMEOUT(reply.isNull(), 1000);
    }

    void cancel()
    {
        QTcpServer server;
        QList<QByteArray> requests;
        serve(server, {}, requests, -1);
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        platform_client client;
        QVERIFY(client.set_base_url(base_url(server)));
        int calls = 0;
        platform_response response;
        QPointer<QNetworkReply> reply = client.request("GET",
                                                       QStringLiteral("/api/devices"),
                                                       {},
                                                       this,
                                                       [&](platform_response value)
                                                       {
                                                           response = std::move(value);
                                                           ++calls;
                                                       });
        QVERIFY(reply);
        reply->abort();
        QTRY_COMPARE_WITH_TIMEOUT(calls, 1, 1000);
        QVERIFY(!response.ok());
        QCOMPARE(response.network_error, QNetworkReply::OperationCanceledError);
        QTRY_VERIFY_WITH_TIMEOUT(reply.isNull(), 1000);
    }

    void destroyed_context()
    {
        QTcpServer server;
        QList<QByteArray> requests;
        serve(server, http_response(200, "{}"), requests, 80);
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        platform_client client;
        QVERIFY(client.set_base_url(base_url(server)));
        auto* context = new QObject;
        bool called = false;
        QPointer<QNetworkReply> reply = client.request("GET", QStringLiteral("/api/devices"), {}, context, [&](platform_response) { called = true; });
        QVERIFY(reply);
        QTRY_COMPARE_WITH_TIMEOUT(requests.size(), 1, 1000);
        delete context;
        QTRY_VERIFY_WITH_TIMEOUT(reply.isNull(), 1000);
        QVERIFY(!called);
    }

    void destroyed_owner()
    {
        QTcpServer server;
        QList<QByteArray> requests;
        serve(server, {}, requests, -1);
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        auto* client = new platform_client;
        QVERIFY(client->set_base_url(base_url(server)));
        bool called = false;
        QPointer<QNetworkReply> reply = client->request("GET", QStringLiteral("/api/devices"), {}, this, [&](platform_response) { called = true; });
        QVERIFY(reply);
        QTRY_COMPARE_WITH_TIMEOUT(requests.size(), 1, 1000);
        delete client;
        QVERIFY(reply.isNull());
        QVERIFY(!called);
    }

    void base_validation()
    {
        QVERIFY(platform_client::valid_base_url(QUrl(QStringLiteral("https://example.invalid/gateway"))));
        for (const auto& input : {"",
                                  "relative/path",
                                  "ftp://example.invalid",
                                  "http:///path",
                                  "http://user:dummy@example.invalid",
                                  "http://example.invalid/?x=1",
                                  "http://example.invalid/#part",
                                  "http://example.invalid:0"})
        {
            QVERIFY(!platform_client::valid_base_url(QUrl(QString::fromLatin1(input))));
        }
        platform_client client;
        QVERIFY(client.set_base_url(QUrl(QStringLiteral("http://example.invalid/gateway"))));
        QVERIFY(!client.set_base_url(QUrl(QStringLiteral("relative"))));
        bool called = false;
        platform_response response;
        QVERIFY(!client.request("GET",
                                QStringLiteral("/api/devices"),
                                {},
                                this,
                                [&](platform_response value)
                                {
                                    response = std::move(value);
                                    called = true;
                                }));
        QVERIFY(!called);
        QTRY_VERIFY_WITH_TIMEOUT(called, 1000);
        QVERIFY(!response.ok());
        QCOMPARE(response.network_error, QNetworkReply::ProtocolInvalidOperationError);
    }
};

}    // namespace media_server

QTEST_GUILESS_MAIN(media_server::platform_client_test)
#include "platform_client_test.moc"
