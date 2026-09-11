#ifndef MEDIA_HTTP_WHIP_HTTP_H
#define MEDIA_HTTP_WHIP_HTTP_H

#include <string>

#include <boost/beast/http.hpp>
#include <boost/url/url_view.hpp>

#include "config.h"

namespace media_server
{
class worker_context;

using whip_http_request = boost::beast::http::request<boost::beast::http::string_body>;
using whip_http_string_response = boost::beast::http::response<boost::beast::http::string_body>;

[[nodiscard]] whip_http_string_response handle_whip_request(const whip_http_request& request,
                                                            worker_context& worker,
                                                            const boost::urls::url_view& target,
                                                            const config& application_config);

}    // namespace media_server

#endif
