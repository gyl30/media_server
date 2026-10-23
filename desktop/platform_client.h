#ifndef DESKTOP_PLATFORM_CLIENT_H
#define DESKTOP_PLATFORM_CLIENT_H

#include <functional>

#include <QUrl>
#include <QObject>
#include <QString>
#include <QByteArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QNetworkReply>

class QNetworkAccessManager;

namespace media_server
{

struct platform_response
{
    int status{};
    QNetworkReply::NetworkError network_error{QNetworkReply::NoError};
    QJsonDocument document;
    QString error;

    bool ok() const;
};

class platform_client final : public QObject
{
   public:
    explicit platform_client(QObject* parent = nullptr);

   public:
    static bool valid_base_url(const QUrl& url);
    bool set_base_url(const QUrl& url);
    QUrl base_url() const;
    QNetworkReply* request(const QByteArray& method,
                           const QString& path,
                           const QJsonObject& body,
                           QObject* context,
                           std::function<void(platform_response)> callback,
                           int timeout_ms = 10'000);

   private:
    QUrl base_url_;
    QNetworkAccessManager* manager_;
};

}    // namespace media_server

#endif
