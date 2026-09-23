#include <array>
#include <utility>
#include <optional>

#include <boost/url/parse.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/write.hpp>
#include <boost/asio/detached.hpp>
#include <boost/beast/http/chunk_encode.hpp>

#include "media/core/stream_id.h"
#include "media/http/http_event.h"
#include "media/net/worker_context.h"
#include "media/core/stream_registry.h"
#include "media/http/http_flv_session.h"
#include "media/http/signaling_client.h"
#include "media/http/http_flv_streamer.h"

namespace media_server
{

http_flv_session::http_flv_session(worker_context& worker, boost::beast::tcp_stream stream, request_type request, const config& config)
    : worker_(worker), stream_(std::move(stream)), request_(std::move(request)), config_(config)
{
}

void http_flv_session::startup()
{
    const auto self = shared_from_this();
    worker_.spawn([self](boost::asio::yield_context yield) { self->run(yield); });
}

void http_flv_session::run(boost::asio::yield_context yield)
{
    if (!closed_)
    {
        handle_request(yield);
    }
    shutdown();
}

void http_flv_session::handle_request(boost::asio::yield_context& yield)
{
    if (request_.method() != boost::beast::http::verb::get)
    {
        send_text_response(boost::beast::http::status::method_not_allowed, "text/plain", "method not allowed\n", yield, "GET");
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
        send_text_response(boost::beast::http::status::not_found, "text/plain", "not found\n", yield);
        return;
    }

    path.back().resize(path.back().size() - 4);
    stream_name_.clear();
    for (const auto& segment : path)
    {
        if (!stream_name_.empty())
        {
            stream_name_.push_back('/');
        }
        stream_name_.append(segment);
    }

    std::optional<std::string> stream_id;
    for (const auto parameter : target.params())
    {
        if (parameter.key != "stream_id")
        {
            continue;
        }
        if (stream_id || !parameter.has_value)
        {
            send_text_response(boost::beast::http::status::bad_request, "text/plain", "invalid stream id\n", yield);
            return;
        }
        stream_id = parameter.value;
    }
    if (!stream_id || !valid_stream_id(*stream_id) || stream_name_.empty())
    {
        send_text_response(boost::beast::http::status::bad_request, "text/plain", "invalid stream id\n", yield);
        return;
    }

    const auto claim = signaling_client::instance().claim_play(*stream_id, "http-flv", stream_name_, yield);
    if (claim.kind != signaling_result_kind::accepted)
    {
        const auto status =
            claim.kind == signaling_result_kind::rejected ? boost::beast::http::status::forbidden : boost::beast::http::status::service_unavailable;
        send_text_response(status, "text/plain", "play claim failed\n", yield);
        return;
    }

    auto media_stream = stream_registry::instance().find(stream_name_);
    if (!media_stream)
    {
        send_text_response(boost::beast::http::status::not_found, "text/plain", "stream not found\n", yield);
        return;
    }

    stream_id_ = std::move(*stream_id);
    http_event::report_flv_output(event_state::starting, stream_id_, stream_name_, "play");

    const auto self = shared_from_this();
    streamer_ = std::make_shared<http_flv_streamer>(
        [self](std::uint64_t generation, std::vector<std::uint8_t> data, bool bootstrap) { self->enqueue(generation, std::move(data), bootstrap); },
        [self]()
        {
            http_event::report_flv_output(event_state::remote_closed, self->stream_id_, self->stream_name_, "media");
            self->shutdown();
        },
        config_.http_video);

    stream_.expires_never();
    {
        boost::beast::http::response<boost::beast::http::empty_body> response(boost::beast::http::status::ok, request_.version());
        response.set(boost::beast::http::field::server, "media_server");
        response.set(boost::beast::http::field::content_type, "video/x-flv");
        response.set(boost::beast::http::field::cache_control, "no-cache");
        response.keep_alive(false);
        response.chunked(true);

        boost::beast::http::serializer<false, boost::beast::http::empty_body> serializer(response);
        boost::system::error_code error;
        boost::beast::http::async_write_header(stream_, serializer, yield[error]);
        if (error)
        {
            if (yield.cancelled() == boost::asio::cancellation_type::none)
            {
                http_event::report_flv_output(event_state::runtime_error, stream_id_, stream_name_, "transport", error.message());
            }
            return;
        }
        if (closed_)
        {
            return;
        }
    }

    media_stream->add_reader(streamer_, worker_);
    http_event::report_flv_output(event_state::streaming, stream_id_, stream_name_, "streaming");

    std::array<std::uint8_t, 1> read_buffer{};
    for (;;)
    {
        boost::system::error_code error;
        stream_.async_read_some(boost::asio::buffer(read_buffer), yield[error]);
        if (error)
        {
            if (yield.cancelled() == boost::asio::cancellation_type::none)
            {
                http_event::report_flv_output(event_state::runtime_error, stream_id_, stream_name_, "transport", error.message());
            }
            return;
        }
        if (closed_)
        {
            return;
        }
    }
}

void http_flv_session::send_text_response(
    boost::beast::http::status status, std::string_view content_type, std::string body, boost::asio::yield_context& yield, std::string_view allow)
{
    boost::beast::http::response<boost::beast::http::string_body> response(status, request_.version());
    response.set(boost::beast::http::field::server, "media_server");
    response.set(boost::beast::http::field::content_type, content_type);
    if (!allow.empty())
    {
        response.set(boost::beast::http::field::allow, allow);
    }
    response.keep_alive(false);
    response.body() = std::move(body);
    response.prepare_payload();

    boost::system::error_code error;
    if (request_.method() == boost::beast::http::verb::head)
    {
        boost::beast::http::response_serializer<boost::beast::http::string_body> serializer(response);
        boost::beast::http::async_write_header(stream_, serializer, yield[error]);
        return;
    }
    boost::beast::http::async_write(stream_, response, yield[error]);
}

void http_flv_session::enqueue(std::uint64_t generation, std::vector<std::uint8_t> data, bool bootstrap)
{
    if (write_in_progress_)
    {
        if (!bootstrap)
        {
            http_event::report_flv_output(event_state::runtime_error, stream_id_, stream_name_, "transport", "write_queue_overflow");
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
        streamer_->write_complete(generation);
        return;
    }

    write_in_progress_ = true;
    const auto self = shared_from_this();
    worker_.spawn(
        [self, generation, data = std::move(data)](boost::asio::yield_context yield) mutable { self->run_write(generation, std::move(data), yield); });
}

void http_flv_session::run_write(std::uint64_t generation, std::vector<std::uint8_t> data, boost::asio::yield_context yield)
{
    for (;;)
    {
        const auto chunk = boost::beast::http::make_chunk(boost::asio::buffer(data));
        boost::system::error_code error;
        boost::asio::async_write(stream_, chunk, yield[error]);
        if (error)
        {
            write_in_progress_ = false;
            if (yield.cancelled() == boost::asio::cancellation_type::none)
            {
                http_event::report_flv_output(event_state::runtime_error, stream_id_, stream_name_, "transport", error.message());
            }
            shutdown();
            return;
        }
        if (closed_)
        {
            write_in_progress_ = false;
            shutdown();
            return;
        }

        if (!pending_bootstrap_ready_)
        {
            write_in_progress_ = false;
            streamer_->write_complete(generation);
            return;
        }

        generation = pending_generation_;
        data = std::move(pending_bootstrap_);
        pending_bootstrap_ready_ = false;
        if (data.empty())
        {
            write_in_progress_ = false;
            streamer_->write_complete(generation);
            return;
        }
    }
}

void http_flv_session::shutdown()
{
    const auto self = shared_from_this();
    boost::asio::post(worker_.io(), [self]() { self->safe_shutdown(); });
}

void http_flv_session::safe_shutdown()
{
    if (closed_)
    {
        return;
    }
    closed_ = true;
    if (streamer_)
    {
        http_event::report_flv_output(event_state::stopped, stream_id_, stream_name_);
        streamer_->shutdown();
        streamer_.reset();
    }
    pending_bootstrap_.clear();
    pending_bootstrap_ready_ = false;
    write_in_progress_ = false;
    boost::system::error_code error;
    stream_.socket().shutdown(boost::asio::ip::tcp::socket::shutdown_both, error);
    stream_.socket().close(error);
}

}    // namespace media_server
