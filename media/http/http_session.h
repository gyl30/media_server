#ifndef MEDIA_HTTP_HTTP_SESSION_H
#define MEDIA_HTTP_HTTP_SESSION_H

#include <memory>
#include <string>

#include <boost/asio/spawn.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/asio/ip/tcp.hpp>

#include "config.h"

namespace media_server
{
class worker_context;
class io_context_pool;

class http_session final : public std::enable_shared_from_this<http_session>
{
   public:
    http_session(worker_context& worker, boost::asio::ip::tcp::socket socket, io_context_pool& workers, const config& config);

    void startup();
    void shutdown();

   private:
    void run(boost::asio::yield_context yield);
    void handle_request(boost::beast::http::request<boost::beast::http::string_body>& request, boost::asio::yield_context yield);
    void write_response(boost::beast::http::request<boost::beast::http::string_body>& request,
                        boost::beast::http::response<boost::beast::http::string_body> response,
                        boost::asio::yield_context yield);
    void write_string_response(boost::beast::http::request<boost::beast::http::string_body>& request,
                               boost::beast::http::response<boost::beast::http::string_body> response,
                               boost::asio::yield_context yield);
    void send_text_response(boost::beast::http::request<boost::beast::http::string_body>& request,
                            boost::beast::http::status status,
                            std::string_view content_type,
                            std::string body,
                            boost::asio::yield_context yield,
                            std::string_view allow = {});
    void safe_shutdown();

    worker_context& worker_;
    boost::beast::tcp_stream stream_;
    io_context_pool& workers_;
    const config& config_;
    bool closed_{};
};
}    // namespace media_server

#endif
