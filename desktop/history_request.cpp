#include <utility>

#include <QDebug>
#include <QPointer>
#include <QDateTime>

#include "desktop/history_request.h"
#include "desktop/platform_client.h"

namespace media_server
{

void request_history(platform_client& client,
                     const QString& path,
                     const QJsonObject& body,
                     QObject* consumer,
                     std::function<void(const QUrl& url, platform_client* owner, const QString& id, QDeadlineTimer deadline)> ready,
                     std::function<void(const platform_response& response)> failed)
{
    const QPointer<QObject> session = consumer;
    const QPointer<platform_client> owner = new platform_client(&client);
    owner->set_base_url(client.base_url());
    auto* response_context = new QObject(owner);
    owner->request(
        "POST",
        path,
        body,
        response_context,
        [session, owner, response_context, ready = std::move(ready), failed = std::move(failed)](platform_response response)
        {
            response_context->deleteLater();
            const auto object = response.document.object();
            const auto id = object.value(QStringLiteral("playback_id")).toString();
            const auto release = [owner, id]()
            {
                if (!owner)
                {
                    return;
                }
                if (id.isEmpty())
                {
                    owner->deleteLater();
                    return;
                }
                owner->request("DELETE",
                               QStringLiteral("/api/playbacks/%1").arg(QString::fromLatin1(QUrl::toPercentEncoding(id))),
                               {},
                               owner,
                               [owner](platform_response result)
                               {
                                   if (!result.ok() && result.status != 404)
                                   {
                                       qWarning().noquote() << QStringLiteral("独占录像实例回收失败：") + result.error;
                                   }
                                   owner->deleteLater();
                               });
            };
            const QUrl url(object.value(QStringLiteral("ps_url")).toString(), QUrl::StrictMode);
            const auto end = QDateTime::fromString(object.value(QStringLiteral("deadline")).toString(), Qt::ISODateWithMs);
            const auto remaining = QDateTime::currentDateTimeUtc().msecsTo(end);
            const bool valid = response.ok() && response.status == 201 && !id.isEmpty() && url.isValid() && !url.isRelative() &&
                               (url.scheme() == QStringLiteral("http") || url.scheme() == QStringLiteral("https")) && !url.host().isEmpty() &&
                               !url.authority(QUrl::FullyEncoded).contains(QLatin1Char('@')) && !url.hasQuery() && !url.hasFragment() &&
                               url.path().startsWith(QStringLiteral("/play/gb28181/%1/").arg(id)) && url.path().endsWith(QStringLiteral(".ps")) &&
                               end.isValid() && end.timeSpec() != Qt::LocalTime && remaining > 0;
            if (!session || !valid)
            {
                release();
                if (session)
                {
                    failed(response);
                }
                return;
            }
            QObject::connect(session, &QObject::destroyed, owner, release);
            ready(url, owner, id, QDeadlineTimer(remaining, Qt::PreciseTimer));
        },
        20'000);
}

}    // namespace media_server
