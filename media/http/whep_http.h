#ifndef MEDIA_HTTP_WHEP_HTTP_H
#define MEDIA_HTTP_WHEP_HTTP_H

#include <span>
#include <string>
#include <string_view>

#include <boost/asio/any_io_executor.hpp>
#include <boost/beast/http.hpp>

#include "config.h"

namespace media_server
{

using whep_http_request = boost::beast::http::request<boost::beast::http::string_body>;
using whep_http_string_response = boost::beast::http::response<boost::beast::http::string_body>;

[[nodiscard]] whep_http_string_response make_whep_error_response(const whep_http_request& request,
                                                                 boost::beast::http::status status,
                                                                 std::string body,
                                                                 int retry_after_seconds = 0,
                                                                 std::string_view allow = {});
[[nodiscard]] whep_http_string_response handle_whep_request(const whep_http_request& request,
                                                            boost::asio::any_io_executor executor,
                                                            std::span<const std::string> segments,
                                                            const config& application_config);
[[nodiscard]] whep_http_string_response handle_whep_options(const whep_http_request& request, bool session_resource);
[[nodiscard]] whep_http_string_response handle_whep_endpoint_get(const whep_http_request& request);
[[nodiscard]] whep_http_string_response handle_whep_session_get(const whep_http_request& request, std::string_view session_id);
[[nodiscard]] whep_http_string_response handle_whep_post(const whep_http_request& request,
                                                         boost::asio::any_io_executor executor,
                                                         std::string stream_name,
                                                         const config& application_config);
[[nodiscard]] whep_http_string_response handle_whep_delete(const whep_http_request& request, std::string_view session_id);

}    // namespace media_server

#endif
