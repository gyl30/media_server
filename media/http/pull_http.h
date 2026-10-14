#ifndef MEDIA_HTTP_PULL_HTTP_H
#define MEDIA_HTTP_PULL_HTTP_H

#include <ada.h>
#include <boost/beast/http.hpp>

namespace media_server
{
class worker_context;

using pull_http_request = boost::beast::http::request<boost::beast::http::string_body>;
using pull_http_response = boost::beast::http::response<boost::beast::http::string_body>;

// POST /pull/create：按 URL 协议创建主动拉流接收会话（rtsp://、rtmp://）。
[[nodiscard]] pull_http_response handle_pull_request(const pull_http_request& request, worker_context& worker, const ada::url_aggregator& target);

}    // namespace media_server

#endif
