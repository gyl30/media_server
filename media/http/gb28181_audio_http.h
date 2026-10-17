#ifndef MEDIA_HTTP_GB28181_AUDIO_HTTP_H
#define MEDIA_HTTP_GB28181_AUDIO_HTTP_H

#include <ada.h>
#include <boost/asio/spawn.hpp>
#include <boost/beast/http.hpp>
#include <boost/asio/ip/address.hpp>

namespace media_server
{
class worker_context;

[[nodiscard]] boost::beast::http::response<boost::beast::http::string_body> handle_gb28181_audio_request(
    const boost::beast::http::request<boost::beast::http::string_body>& request,
    worker_context& worker,
    const ada::url_aggregator& target,
    boost::asio::ip::address bind_address,
    boost::asio::yield_context yield);

}    // namespace media_server

#endif
