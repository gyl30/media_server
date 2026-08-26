#ifndef MEDIA_HTTP_WHEP_HTTP_H
#define MEDIA_HTTP_WHEP_HTTP_H

#include <string>
#include <string_view>

#include <boost/beast/http.hpp>
#include <boost/url/url_view.hpp>
#include <boost/asio/any_io_executor.hpp>

#include "config.h"

namespace media_server
{

using whep_http_request = boost::beast::http::request<boost::beast::http::string_body>;
using whep_http_string_response = boost::beast::http::response<boost::beast::http::string_body>;

[[nodiscard]] whep_http_string_response handle_whep_request(const whep_http_request& request,
                                                            boost::asio::any_io_executor executor,
                                                            const boost::urls::url_view& target,
                                                            const config& application_config);

}    // namespace media_server

#endif
