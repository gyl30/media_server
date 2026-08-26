#ifndef MEDIA_HTTP_HTTP_SESSION_H
#define MEDIA_HTTP_HTTP_SESSION_H

#include <memory>
#include <string>

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/asio/ip/tcp.hpp>

#include "config.h"

namespace media_server
{
class io_context_pool;

class http_session final : public std::enable_shared_from_this<http_session>
{
   public:
    http_session(boost::asio::ip::tcp::socket socket, io_context_pool& workers, const config& config);

    void startup();
    void shutdown();

   private:
    void read_request();
    void on_request(boost::system::error_code error, std::size_t bytes);
    void handle_request();
    void write_response(boost::beast::http::response<boost::beast::http::string_body> response);
    void write_string_response(std::shared_ptr<boost::beast::http::response<boost::beast::http::string_body>> response);
    void send_text_response(boost::beast::http::status status, std::string_view content_type, std::string body, std::string_view allow = {});
    void safe_shutdown();

    boost::beast::tcp_stream stream_;
    boost::beast::flat_buffer buffer_;
    boost::beast::http::request<boost::beast::http::string_body> request_;
    io_context_pool& workers_;
    const config& config_;
    bool closed_{};
};
}    // namespace media_server

#endif
