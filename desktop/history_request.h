#ifndef DESKTOP_HISTORY_REQUEST_H
#define DESKTOP_HISTORY_REQUEST_H

#include <functional>

#include <QUrl>
#include <QString>
#include <QJsonObject>
#include <QDeadlineTimer>

#include "desktop/platform_client.h"

namespace media_server
{

void request_history(platform_client& client,
                     const QString& path,
                     const QJsonObject& body,
                     QObject* consumer,
                     std::function<void(const QUrl& url, platform_client* owner, const QString& id, QDeadlineTimer deadline)> ready,
                     std::function<void(const platform_response& response)> failed);

}

#endif
