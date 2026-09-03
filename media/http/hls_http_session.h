#ifndef MEDIA_HTTP_HLS_HTTP_SESSION_H
#define MEDIA_HTTP_HLS_HTTP_SESSION_H

#include <chrono>
#include <memory>
#include <string>
#include <vector>
#include <cstdint>
#include <string_view>

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/asio/steady_timer.hpp>

#include "config.h"

namespace media_server
{
class worker_context;

class hls_http_session final : public std::enable_shared_from_this<hls_http_session>
{
   public:
    using request_type = boost::beast::http::request<boost::beast::http::string_body>;

    hls_http_session(worker_context& worker, boost::beast::tcp_stream stream, request_type request, const config& config);

    void startup();
    void shutdown();

   private:
    void handle_request();
    void check_playlist();
    void write_string_response(std::shared_ptr<boost::beast::http::response<boost::beast::http::string_body>> response);
    void send_text_response(boost::beast::http::status status, std::string_view content_type, std::string body, std::string_view allow = {});
    void send_binary_response(boost::beast::http::status status, std::string_view content_type, std::vector<std::uint8_t> body);
    void safe_shutdown();

    worker_context& worker_;
    boost::beast::tcp_stream stream_;
    request_type request_;
    const config& config_;
    boost::asio::steady_timer wait_timer_;
    std::chrono::steady_clock::time_point wait_deadline_{};
    std::string wait_stream_name_;
    bool closed_{};
};

}    // namespace media_server

#endif
