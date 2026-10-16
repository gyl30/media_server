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

// 申请一个独占历史实例（回放或下载）。平台地址在调用时固定；consumer 析构、响应无效或失败时只 DELETE 本实例，
// 迟到的创建响应同样回收。ready 只在 consumer 仍存活且响应有效时调用，owner 为固定平台的客户端，供后续控制；
// failed 只在 consumer 仍存活时调用，response.ok() 为真表示平台返回的实例或媒体地址无效。
void request_history(platform_client& client,
                     const QString& path,
                     const QJsonObject& body,
                     QObject* consumer,
                     std::function<void(const QUrl& url, platform_client* owner, const QString& id, QDeadlineTimer deadline)> ready,
                     std::function<void(const platform_response& response)> failed);

}    // namespace media_server

#endif
