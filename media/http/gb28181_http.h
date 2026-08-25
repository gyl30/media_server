#ifndef MEDIA_HTTP_GB28181_HTTP_H
#define MEDIA_HTTP_GB28181_HTTP_H

#include <span>
#include <string>
#include <string_view>

#include <boost/asio/io_context.hpp>
#include <boost/beast/http.hpp>
#include <boost/url/url_view.hpp>

#include "media/gb28181/gb28181_types.h"

namespace media_server
{

using gb28181_http_request = boost::beast::http::request<boost::beast::http::string_body>;
using gb28181_http_response = boost::beast::http::response<boost::beast::http::string_body>;

[[nodiscard]] gb28181_http_response make_gb28181_error_response(const gb28181_http_request& request,
                                                                boost::beast::http::status status,
                                                                std::string_view error,
                                                                std::string_view allow = {});

[[nodiscard]] gb28181_http_response handle_gb28181_input_request(const gb28181_http_request& request,
                                                                 boost::asio::io_context& owner,
                                                                 const boost::urls::url_view& target,
                                                                 std::span<const std::string> segments);
[[nodiscard]] gb28181_http_response handle_gb28181_output_request(const gb28181_http_request& request,
                                                                  boost::asio::io_context& owner,
                                                                  const boost::urls::url_view& target,
                                                                  std::span<const std::string> segments);

[[nodiscard]] gb28181_http_response handle_gb28181_input_create(const gb28181_http_request& request,
                                                                boost::asio::io_context& owner,
                                                                gb28181_input_config config);
[[nodiscard]] gb28181_http_response handle_gb28181_input_delete(const gb28181_http_request& request, std::string stream_name);
[[nodiscard]] gb28181_http_response handle_gb28181_output_create(const gb28181_http_request& request,
                                                                 boost::asio::io_context& owner,
                                                                 gb28181_output_config config);
[[nodiscard]] gb28181_http_response handle_gb28181_output_delete(const gb28181_http_request& request,
                                                                 std::string stream_name,
                                                                 std::string output_id);

}    // namespace media_server

#endif
