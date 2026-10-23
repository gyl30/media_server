#include <utility>

#include <QTimer>
#include <QThread>
#include <QVariant>
#include <QJsonParseError>
#include <QNetworkRequest>
#include <QNetworkAccessManager>

#include "desktop/platform_client.h"

namespace media_server
{

bool platform_response::ok() const { return error.isEmpty() && network_error == QNetworkReply::NoError && status >= 200 && status < 300; }

platform_client::platform_client(QObject* parent) : QObject(parent), manager_(new QNetworkAccessManager(this)) {}

bool platform_client::valid_base_url(const QUrl& url)
{
    return url.isValid() && !url.isRelative() && (url.scheme() == QStringLiteral("http") || url.scheme() == QStringLiteral("https")) &&
           !url.host().isEmpty() && !url.authority(QUrl::FullyEncoded).contains(QLatin1Char('@')) && !url.hasQuery() && !url.hasFragment() &&
           url.port() != 0;
}

bool platform_client::set_base_url(const QUrl& url)
{
    Q_ASSERT(thread() == QThread::currentThread());
    base_url_ = valid_base_url(url) ? url.adjusted(QUrl::StripTrailingSlash) : QUrl{};
    return !base_url_.isEmpty();
}

QUrl platform_client::base_url() const { return base_url_; }

QNetworkReply* platform_client::request(const QByteArray& method,
                                        const QString& path,
                                        const QJsonObject& body,
                                        QObject* context,
                                        std::function<void(platform_response)> callback,
                                        int timeout_ms)
{
    Q_ASSERT(thread() == QThread::currentThread());
    Q_ASSERT(context && context->thread() == thread());
    if (!context || !callback)
    {
        return nullptr;
    }
    const auto verb = method.toUpper();
    const QUrl endpoint(path, QUrl::StrictMode);
    const auto segments = endpoint.path().split(QLatin1Char('/'));
    const bool valid_endpoint = endpoint.isValid() && endpoint.isRelative() && path.startsWith(QLatin1Char('/')) &&
                                !path.startsWith(QStringLiteral("//")) && !endpoint.hasQuery() && !endpoint.hasFragment() &&
                                !segments.contains(QStringLiteral("..")) && !segments.contains(QStringLiteral("."));
    if (!valid_base_url(base_url_) || !valid_endpoint || timeout_ms <= 0 || (verb != "GET" && verb != "POST" && verb != "PATCH" && verb != "DELETE"))
    {
        QTimer::singleShot(0,
                           context,
                           [callback = std::move(callback)]() mutable
                           {
                               platform_response response;
                               response.network_error = QNetworkReply::ProtocolInvalidOperationError;
                               response.error = QStringLiteral("Invalid platform URL, API path, method or timeout");
                               callback(std::move(response));
                           });
        return nullptr;
    }

    auto url = base_url_;
    url.setPath(base_url_.path() + endpoint.path());
    QNetworkRequest outgoing(url);
    outgoing.setRawHeader("Accept", "application/json");
    outgoing.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    outgoing.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    const auto data = verb == "GET" ? QByteArray{} : QJsonDocument(body).toJson(QJsonDocument::Compact);
    auto* reply = manager_->sendCustomRequest(outgoing, verb, data);
    auto* timer = new QTimer(reply);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, reply, &QNetworkReply::abort);
    connect(reply,
            &QNetworkReply::finished,
            context,
            [reply, timer, callback = std::move(callback)]() mutable
            {
                const bool timed_out = !timer->isActive();
                timer->stop();
                platform_response response;
                response.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                response.network_error = timed_out ? QNetworkReply::TimeoutError : reply->error();
                const auto bytes = reply->isOpen() ? reply->readAll() : QByteArray{};
                QJsonParseError parse_error;
                if (!bytes.isEmpty())
                {
                    response.document = QJsonDocument::fromJson(bytes, &parse_error);
                }
                if (timed_out)
                {
                    response.error = QStringLiteral("Request timed out");
                }
                else if (response.status != 0 && (response.status < 200 || response.status >= 300))
                {
                    response.error = QStringLiteral("HTTP %1").arg(response.status);
                    const auto code = response.document.object().value(QStringLiteral("error")).toString();
                    if (!code.isEmpty())
                    {
                        response.error += QStringLiteral(" · ") + code;
                    }
                }
                else if (response.network_error != QNetworkReply::NoError)
                {
                    response.error = response.network_error == QNetworkReply::OperationCanceledError ? QStringLiteral("Request cancelled")
                                                                                                     : QStringLiteral("Network request failed");
                }
                else if (response.status == 0)
                {
                    response.error = QStringLiteral("Missing HTTP status");
                }
                else if (parse_error.error != QJsonParseError::NoError || (response.document.isNull() && response.status != 204))
                {
                    response.error = QStringLiteral("Invalid JSON response");
                }
                callback(std::move(response));
            });
    connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
    timer->start(timeout_ms);
    return reply;
}

}    // namespace media_server
