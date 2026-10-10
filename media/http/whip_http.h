#ifndef MEDIA_HTTP_WHIP_HTTP_H
#define MEDIA_HTTP_WHIP_HTTP_H

#include <string>

#include <boost/beast/http.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <ada.h>

#include "config.h"

namespace media_server
{
class worker_context;

using whip_http_request = boost::beast::http::request<boost::beast::http::string_body>;
using whip_http_string_response = boost::beast::http::response<boost::beast::http::string_body>;

[[nodiscard]] whip_http_string_response handle_whip_request(const whip_http_request& request,
                                                            worker_context& worker,
                                                            const ada::url_aggregator& target,
                                                            const config& application_config,
                                                            const boost::asio::ip::tcp::socket& socket,
                                                            boost::asio::yield_context yield);

}    // namespace media_server

#endif
