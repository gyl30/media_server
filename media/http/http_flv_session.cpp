#include <utility>

#include <boost/asio/post.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/http/chunk_encode.hpp>
#include <boost/url/parse.hpp>

#include "media/http/http_flv_session.h"

#include "media/core/stream_registry.h"
#include "media/http/http_flv_output.h"

namespace media_server
{

http_flv_session::http_flv_session(boost::beast::tcp_stream stream,
                                   request_type request,
                                   const config& config)
    : stream_(std::move(stream)), request_(std::move(request)), config_(config)
{
}

void http_flv_session::startup() { handle_request(); }

void http_flv_session::handle_request()
{
    if (request_.method() != boost::beast::http::verb::get)
    {
        send_text_response(boost::beast::http::status::method_not_allowed, "text/plain", "method not allowed\n", "GET");
        return;
    }

    const auto target = boost::urls::parse_origin_form(request_.target()).value();
    std::vector<std::string> path;
    for (const auto segment : target.segments())
    {
        path.emplace_back(segment);
    }
    if (path.empty() || !path.back().ends_with(".flv"))
    {
        send_text_response(boost::beast::http::status::not_found, "text/plain", "not found\n");
        return;
    }

    path.back().resize(path.back().size() - 4);
    std::string stream_name;
    for (const auto& segment : path)
    {
        if (!stream_name.empty())
        {
            stream_name.push_back('/');
        }
        stream_name.append(segment);
    }

    auto media_stream = registry::instance().find(stream_name);
    if (!media_stream)
    {
        send_text_response(boost::beast::http::status::not_found, "text/plain", "stream not found\n");
        return;
    }

    auto response =
        std::make_shared<boost::beast::http::response<boost::beast::http::empty_body>>(boost::beast::http::status::ok, request_.version());
    response->set(boost::beast::http::field::server, "media_server");
    response->set(boost::beast::http::field::content_type, "video/x-flv");
    response->set(boost::beast::http::field::cache_control, "no-cache");
    response->keep_alive(false);
    response->chunked(true);

    auto serializer = std::make_shared<boost::beast::http::serializer<false, boost::beast::http::empty_body>>(*response);
    stream_.expires_never();
    const auto self = shared_from_this();
    boost::beast::http::async_write_header(
        stream_,
        *serializer,
        [self, response, serializer, media_stream = std::move(media_stream)](boost::system::error_code error, std::size_t bytes) mutable
        {
            static_cast<void>(response);
            static_cast<void>(serializer);
            static_cast<void>(bytes);
            if (error)
            {
                self->shutdown();
                return;
            }
            self->startup_flv(std::move(media_stream));
        });
}

void http_flv_session::write_string_response(
    std::shared_ptr<boost::beast::http::response<boost::beast::http::string_body>> response)
{
    const auto self = shared_from_this();
    if (request_.method() == boost::beast::http::verb::head)
    {
        auto serializer = std::make_shared<boost::beast::http::response_serializer<boost::beast::http::string_body>>(*response);
        boost::beast::http::async_write_header(
            stream_,
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

    boost::beast::http::async_write(
        stream_,
        *response,
        [self, response](boost::system::error_code error, std::size_t bytes)
        {
            static_cast<void>(response);
            static_cast<void>(error);
            static_cast<void>(bytes);
            self->shutdown();
        });
}

void http_flv_session::send_text_response(boost::beast::http::status status,
                                          std::string_view content_type,
                                          std::string body,
                                          std::string_view allow)
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

void http_flv_session::startup_flv(std::shared_ptr<media_stream> media_stream)
{
    if (closed_)
    {
        return;
    }

    const auto weak = weak_from_this();
    output_ = std::make_shared<http_flv_output>(
        [weak](std::uint64_t generation, std::vector<std::uint8_t> data, bool bootstrap)
        {
            if (const auto self = weak.lock())
            {
                self->enqueue(generation, std::move(data), bootstrap);
            }
        },
        [weak]()
        {
            if (const auto self = weak.lock())
            {
                self->shutdown();
            }
        },
        config_.http_video);

    reader_ = media_stream->add_reader(output_, stream_.get_executor());
    read_client();
}

void http_flv_session::read_client()
{
    if (closed_)
    {
        return;
    }

    const auto self = shared_from_this();
    stream_.async_read_some(
        boost::asio::buffer(read_buffer_),
        [self](boost::system::error_code error, std::size_t bytes)
        {
            static_cast<void>(bytes);
            if (error)
            {
                self->shutdown();
                return;
            }
            self->read_client();
        });
}

void http_flv_session::enqueue(std::uint64_t generation, std::vector<std::uint8_t> data, bool bootstrap)
{
    if (closed_)
    {
        return;
    }

    if (write_in_progress_)
    {
        if (!bootstrap)
        {
            shutdown();
            return;
        }
        pending_generation_ = generation;
        pending_bootstrap_ = std::move(data);
        pending_bootstrap_ready_ = true;
        return;
    }

    if (data.empty())
    {
        output_->write_complete(generation);
        return;
    }
    write_chunk(generation, std::move(data));
}

void http_flv_session::write_chunk(std::uint64_t generation, std::vector<std::uint8_t> data)
{
    write_in_progress_ = true;
    const auto buffer = std::make_shared<std::vector<std::uint8_t>>(std::move(data));
    const auto chunk = std::make_shared<decltype(boost::beast::http::make_chunk(boost::asio::buffer(*buffer)))>(
        boost::beast::http::make_chunk(boost::asio::buffer(*buffer)));
    const auto self = shared_from_this();
    boost::asio::async_write(
        stream_,
        *chunk,
        [self, buffer, chunk, generation](boost::system::error_code error, std::size_t bytes)
        {
            static_cast<void>(buffer);
            static_cast<void>(bytes);
            self->on_write(generation, error);
        });
}

void http_flv_session::on_write(std::uint64_t generation, boost::system::error_code error)
{
    if (closed_)
    {
        return;
    }
    write_in_progress_ = false;
    if (error)
    {
        shutdown();
        return;
    }
    if (pending_bootstrap_ready_)
    {
        const auto pending_generation = pending_generation_;
        auto pending = std::move(pending_bootstrap_);
        pending_bootstrap_ready_ = false;
        if (pending.empty())
        {
            output_->write_complete(pending_generation);
        }
        else
        {
            write_chunk(pending_generation, std::move(pending));
        }
        return;
    }
    output_->write_complete(generation);
}

void http_flv_session::detach()
{
    reader_.remove();
    reader_ = {};
    output_.reset();
    pending_bootstrap_.clear();
    pending_bootstrap_ready_ = false;
    write_in_progress_ = false;
}

void http_flv_session::shutdown()
{
    const auto self = shared_from_this();
    boost::asio::post(stream_.get_executor(), [self]() { self->safe_shutdown(); });
}

void http_flv_session::safe_shutdown()
{
    if (closed_)
    {
        return;
    }
    closed_ = true;
    detach();
    boost::system::error_code error;
    stream_.socket().shutdown(boost::asio::ip::tcp::socket::shutdown_both, error);
    stream_.socket().close(error);
}

}    // namespace media_server
