#include <chrono>
#include <utility>

#include <boost/asio/post.hpp>
#include <boost/url/parse.hpp>

#include "media/http/whep_http.h"
#include "media/http/gb28181_http.h"
#include "media/http/http_session.h"
#include "media/net/io_context_pool.h"
#include "media/http/hls_http_session.h"
#include "media/http/http_flv_session.h"

namespace media_server
{

http_session::http_session(boost::asio::ip::tcp::socket socket, io_context_pool& workers, const config& config)
    : stream_(std::move(socket)), workers_(workers), config_(config)
{
}

void http_session::startup()
{
    stream_.expires_after(std::chrono::seconds(30));
    read_request();
}

void http_session::read_request()
{
    const auto self = shared_from_this();
    boost::beast::http::async_read(
        stream_, buffer_, request_, [self](boost::system::error_code error, std::size_t bytes) { self->on_request(error, bytes); });
}

void http_session::on_request(boost::system::error_code error, std::size_t bytes)
{
    static_cast<void>(bytes);
    if (closed_)
    {
        return;
    }
    if (error)
    {
        shutdown();
        return;
    }
    handle_request();
}

void http_session::handle_request()
{
    const auto parsed = boost::urls::parse_origin_form(request_.target());
    if (!parsed)
    {
        send_text_response(boost::beast::http::status::bad_request, "text/plain", "bad request target\n");
        return;
    }

    const auto encoded_path = parsed->encoded_path();
    const std::string_view path(encoded_path.data(), encoded_path.size());
    if (path == "/")
    {
        send_text_response(boost::beast::http::status::not_found, "text/plain", "not found\n");
        return;
    }
    if (path == "/gb28181" || path.starts_with("/gb28181/"))
    {
        write_response(media_server::handle_gb28181_input_request(request_, workers_.next(), *parsed));
        return;
    }
    if (path == "/play/gb28181" || path.starts_with("/play/gb28181/"))
    {
        write_response(media_server::handle_gb28181_output_request(request_, workers_.next(), *parsed));
        return;
    }
    if (path == "/play/whep" || path.starts_with("/play/whep/"))
    {
        write_response(media_server::handle_whep_request(request_, stream_.get_executor(), *parsed, config_));
        return;
    }
    if (path == "/play/hls" || path.starts_with("/play/hls/"))
    {
        auto session = std::make_shared<hls_http_session>(std::move(stream_), std::move(request_), config_);
        session->startup();
        return;
    }

    const auto decoded_path = parsed->path();
    if (decoded_path.ends_with(".flv"))
    {
        auto session = std::make_shared<http_flv_session>(std::move(stream_), std::move(request_), config_);
        session->startup();
        return;
    }

    if (request_.method() != boost::beast::http::verb::get)
    {
        send_text_response(boost::beast::http::status::method_not_allowed, "text/plain", "method not allowed\n", "GET");
        return;
    }

    send_text_response(boost::beast::http::status::not_found, "text/plain", "not found\n");
}

void http_session::write_response(boost::beast::http::response<boost::beast::http::string_body> response)
{
    write_string_response(std::make_shared<boost::beast::http::response<boost::beast::http::string_body>>(std::move(response)));
}

void http_session::write_string_response(std::shared_ptr<boost::beast::http::response<boost::beast::http::string_body>> response)
{
    const auto self = shared_from_this();
    if (request_.method() == boost::beast::http::verb::head)
    {
        auto serializer = std::make_shared<boost::beast::http::response_serializer<boost::beast::http::string_body>>(*response);
        boost::beast::http::async_write_header(stream_,
                                               *serializer,
                                               [self, response, serializer](boost::system::error_code error, std::size_t bytes)
                                               {
                                                   static_cast<void>(response);
                                                   static_cast<void>(serializer);
                                                   static_cast<void>(error);
                                                   static_cast<void>(bytes);
                                                   self->shutdown();
                                               });
        return;
    }

    boost::beast::http::async_write(stream_,
                                    *response,
                                    [self, response](boost::system::error_code error, std::size_t bytes)
                                    {
                                        static_cast<void>(response);
                                        static_cast<void>(error);
                                        static_cast<void>(bytes);
                                        self->shutdown();
                                    });
}

void http_session::send_text_response(boost::beast::http::status status, std::string_view content_type, std::string body, std::string_view allow)
{
    auto response = std::make_shared<boost::beast::http::response<boost::beast::http::string_body>>(status, request_.version());
    response->set(boost::beast::http::field::server, "media_server");
    response->set(boost::beast::http::field::content_type, content_type);
    if (!allow.empty())
    {
        response->set(boost::beast::http::field::allow, allow);
    }
    response->keep_alive(false);
    response->body() = std::move(body);
    response->prepare_payload();

    write_string_response(std::move(response));
}

void http_session::shutdown()
{
    const auto self = shared_from_this();
    boost::asio::post(stream_.get_executor(), [self]() { self->safe_shutdown(); });
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
