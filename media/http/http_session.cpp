#include <chrono>
#include <utility>

#include <boost/asio/post.hpp>
#include <boost/asio/detached.hpp>
#include <boost/url/parse.hpp>

#include "media/http/whep_http.h"
#include "media/http/gb28181_http.h"
#include "media/http/http_session.h"
#include "media/net/io_context_pool.h"
#include "media/net/worker_context.h"
#include "media/http/hls_http_session.h"
#include "media/http/http_flv_session.h"

namespace media_server
{

http_session::http_session(worker_context& worker, boost::asio::ip::tcp::socket socket, io_context_pool& workers, const config& config)
    : worker_(worker), stream_(std::move(socket)), workers_(workers), config_(config)
{
}

void http_session::startup()
{
    const auto self = shared_from_this();
    boost::asio::spawn(worker_.io(), [self](boost::asio::yield_context yield) { self->run(yield); }, boost::asio::detached);
}

void http_session::run(boost::asio::yield_context yield)
{
    boost::beast::flat_buffer buffer;
    boost::beast::http::request<boost::beast::http::string_body> request;
    boost::system::error_code error;

    stream_.expires_after(std::chrono::seconds(30));
    static_cast<void>(boost::beast::http::async_read(stream_, buffer, request, yield[error]));
    if (error)
    {
        shutdown();
    }
    else
    {
        handle_request(request, yield);
    }
}

void http_session::handle_request(boost::beast::http::request<boost::beast::http::string_body>& request, boost::asio::yield_context yield)
{
    const auto parsed = boost::urls::parse_origin_form(request.target());
    if (!parsed)
    {
        send_text_response(request, boost::beast::http::status::bad_request, "text/plain", "bad request target\n", yield);
        return;
    }

    const auto encoded_path = parsed->encoded_path();
    const std::string_view path(encoded_path.data(), encoded_path.size());
    if (path == "/")
    {
        send_text_response(request, boost::beast::http::status::not_found, "text/plain", "not found\n", yield);
        return;
    }
    if (path == "/gb28181" || path.starts_with("/gb28181/"))
    {
        write_response(request, media_server::handle_gb28181_input_request(request, workers_.next(), *parsed), yield);
        return;
    }
    if (path == "/play/gb28181" || path.starts_with("/play/gb28181/"))
    {
        write_response(request,
                       media_server::handle_gb28181_output_request(
                           request, workers_.next(), *parsed, boost::asio::ip::make_address(config_.bind_address)),
                       yield);
        return;
    }
    if (path == "/play/whep" || path.starts_with("/play/whep/"))
    {
        write_response(request, media_server::handle_whep_request(request, worker_, *parsed, config_), yield);
        return;
    }
    if (path == "/play/hls" || path.starts_with("/play/hls/"))
    {
        auto session = std::make_shared<hls_http_session>(worker_, std::move(stream_), std::move(request), config_);
        session->startup();
        return;
    }

    const auto decoded_path = parsed->path();
    if (decoded_path.ends_with(".flv"))
    {
        auto session = std::make_shared<http_flv_session>(worker_, std::move(stream_), std::move(request), config_);
        session->startup();
        return;
    }

    if (request.method() != boost::beast::http::verb::get)
    {
        send_text_response(request, boost::beast::http::status::method_not_allowed, "text/plain", "method not allowed\n", yield, "GET");
        return;
    }

    send_text_response(request, boost::beast::http::status::not_found, "text/plain", "not found\n", yield);
}

void http_session::write_response(boost::beast::http::request<boost::beast::http::string_body>& request,
                                  boost::beast::http::response<boost::beast::http::string_body> response,
                                  boost::asio::yield_context yield)
{
    write_string_response(request, std::move(response), yield);
}

void http_session::write_string_response(boost::beast::http::request<boost::beast::http::string_body>& request,
                                         boost::beast::http::response<boost::beast::http::string_body> response,
                                         boost::asio::yield_context yield)
{
    boost::system::error_code error;
    if (request.method() == boost::beast::http::verb::head)
    {
        boost::beast::http::response_serializer<boost::beast::http::string_body> serializer(response);
        static_cast<void>(boost::beast::http::async_write_header(stream_, serializer, yield[error]));
    }
    else
    {
        static_cast<void>(boost::beast::http::async_write(stream_, response, yield[error]));
    }
    shutdown();
}

void http_session::send_text_response(boost::beast::http::request<boost::beast::http::string_body>& request,
                                      boost::beast::http::status status,
                                      std::string_view content_type,
                                      std::string body,
                                      boost::asio::yield_context yield,
                                      std::string_view allow)
{
    boost::beast::http::response<boost::beast::http::string_body> response(status, request.version());
    response.set(boost::beast::http::field::server, "media_server");
    response.set(boost::beast::http::field::content_type, content_type);
    if (!allow.empty())
    {
        response.set(boost::beast::http::field::allow, allow);
    }
    response.keep_alive(false);
    response.body() = std::move(body);
    response.prepare_payload();

    write_string_response(request, std::move(response), yield);
}

void http_session::shutdown()
{
    const auto self = shared_from_this();
    boost::asio::post(worker_.io(), [self]() { self->safe_shutdown(); });
}

void http_session::safe_shutdown()
{
    if (closed_)
    {
        return;
    }
    closed_ = true;
    boost::system::error_code error;
    stream_.socket().shutdown(boost::asio::ip::tcp::socket::shutdown_both, error);
    stream_.socket().close(error);
}

}    // namespace media_server
