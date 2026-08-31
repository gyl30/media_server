#ifndef MEDIA_HTTP_GB28181_HTTP_H
#define MEDIA_HTTP_GB28181_HTTP_H

#include <boost/beast/http.hpp>
#include <boost/url/url_view.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/io_context.hpp>

namespace media_server
{

using gb28181_http_request = boost::beast::http::request<boost::beast::http::string_body>;
using gb28181_http_response = boost::beast::http::response<boost::beast::http::string_body>;

[[nodiscard]] gb28181_http_response handle_gb28181_input_request(const gb28181_http_request& request,
                                                                 boost::asio::io_context& owner,
                                                                 const boost::urls::url_view& target);
[[nodiscard]] gb28181_http_response handle_gb28181_output_request(const gb28181_http_request& request,
                                                                  boost::asio::io_context& owner,
                                                                  const boost::urls::url_view& target,
                                                                  boost::asio::ip::address bind_address);

}    // namespace media_server

#endif
