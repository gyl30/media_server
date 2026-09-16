#ifndef MEDIA_HTTP_RTSP_PULL_HTTP_H
#define MEDIA_HTTP_RTSP_PULL_HTTP_H

#include <boost/beast/http.hpp>
#include <boost/url/url_view.hpp>

namespace media_server
{
class worker_context;

using rtsp_pull_http_request = boost::beast::http::request<boost::beast::http::string_body>;
using rtsp_pull_http_response = boost::beast::http::response<boost::beast::http::string_body>;

[[nodiscard]] rtsp_pull_http_response handle_rtsp_pull_request(const rtsp_pull_http_request& request,
                                                               worker_context& worker,
                                                               const boost::urls::url_view& target);

}    // namespace media_server

#endif
