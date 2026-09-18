#include <chrono>
#include <utility>
#include <charconv>
#include <optional>

#include <boost/asio/post.hpp>
#include <boost/url/parse.hpp>
#include <boost/asio/detached.hpp>

#include "media/hls/hls.h"
#include "media/core/stream_id.h"
#include "media/http/http_event.h"
#include "media/net/worker_context.h"
#include "media/hls/hls_play_session.h"
#include "media/http/hls_http_session.h"
#include "media/http/signaling_client.h"

namespace media_server
{

hls_http_session::hls_http_session(worker_context& worker, boost::beast::tcp_stream stream, request_type request, const config& config)
    : worker_(worker), stream_(std::move(stream)), request_(std::move(request)), config_(config), wait_timer_(worker_.io())
{
}

void hls_http_session::startup()
{
    const auto self = shared_from_this();
    boost::asio::spawn(worker_.io(), [self](boost::asio::yield_context yield) { self->run(yield); }, boost::asio::detached);
}

void hls_http_session::run(boost::asio::yield_context yield)
{
    if (!closed_)
    {
        handle_request(yield);
    }
    shutdown();
}

void hls_http_session::handle_request(boost::asio::yield_context& yield)
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

    if (path.size() < 4 || path[0] != "play" || path[1] != "hls")
    {
        send_text_response(boost::beast::http::status::not_found, "text/plain", "not found\n", yield);
        return;
    }

    const auto& file = path.back();
    std::string stream_name;
    for (std::size_t index = 2; index + 1 < path.size(); ++index)
    {
        if (!stream_name.empty())
        {
            stream_name.push_back('/');
        }
        stream_name.append(path[index]);
    }

    if (stream_name.empty())
    {
        send_text_response(boost::beast::http::status::not_found, "text/plain", "not found\n", yield);
        return;
    }

    std::optional<std::string> stream_id;
    std::optional<std::string> secret;
    for (const auto parameter : target.params())
    {
        if (parameter.key == "stream_id")
        {
            if (stream_id || !parameter.has_value)
            {
                send_text_response(boost::beast::http::status::bad_request, "text/plain", "invalid stream id\n", yield);
                return;
            }
            stream_id = parameter.value;
        }
        else if (parameter.key == "session")
        {
            if (secret || !parameter.has_value)
            {
                send_text_response(boost::beast::http::status::forbidden, "text/plain", "invalid hls session\n", yield);
                return;
            }
            secret = parameter.value;
        }
    }

    if (stream_id)
    {
        if (file != "index.m3u8" || secret || !valid_stream_id(*stream_id))
        {
            send_text_response(boost::beast::http::status::bad_request, "text/plain", "invalid stream id\n", yield);
            return;
        }
        const auto claim = signaling_client::instance().claim_play(*stream_id, "hls", stream_name, yield);
        if (claim.kind != signaling_result_kind::accepted)
        {
            const auto status = claim.kind == signaling_result_kind::rejected ? boost::beast::http::status::forbidden
                                                                              : boost::beast::http::status::service_unavailable;
            send_text_response(status, "text/plain", "play claim failed\n", yield);
            return;
        }

        const auto viewer = hls_play_session::create(worker_, std::move(*stream_id), stream_name);
        http_event::report_hls_output(event_state::starting, viewer->stream_id(), viewer->stream_name(), "play");

        boost::beast::http::response<boost::beast::http::empty_body> response(boost::beast::http::status::temporary_redirect, request_.version());
        response.set(boost::beast::http::field::server, "media_server");
        response.set(boost::beast::http::field::location, std::string(target.encoded_path()) + "?session=" + viewer->secret());
        response.keep_alive(false);
        boost::system::error_code error;
        boost::beast::http::async_write(stream_, response, yield[error]);
        return;
    }

    if (!secret)
    {
        send_text_response(boost::beast::http::status::forbidden, "text/plain", "hls session required\n", yield);
        return;
    }
    const auto viewer = hls_play_session::find(*secret, stream_name);
    if (!viewer)
    {
        send_text_response(boost::beast::http::status::forbidden, "text/plain", "invalid hls session\n", yield);
        return;
    }

    if (file == "index.m3u8")
    {
        auto count = hls::segment_count(stream_name, config_);
        if (!count)
        {
            send_text_response(boost::beast::http::status::not_found, "text/plain", "stream not found\n", yield);
            return;
        }
        if (*count == 0)
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            for (;;)
            {
                count = hls::segment_count(stream_name, config_);
                if (!count)
                {
                    send_text_response(boost::beast::http::status::not_found, "text/plain", "stream not found\n", yield);
                    return;
                }
                if (*count > 0)
                {
                    break;
                }
                if (std::chrono::steady_clock::now() >= deadline)
                {
                    send_text_response(boost::beast::http::status::service_unavailable, "text/plain", "hls playlist not ready\n", yield);
                    return;
                }

                wait_timer_.expires_after(std::chrono::milliseconds(100));
                boost::system::error_code error;
                wait_timer_.async_wait(yield[error]);
                if (error || closed_)
                {
                    return;
                }
            }
        }

        const auto playlist = hls::playlist(stream_name, config_, "session=" + viewer->secret());
        if (!playlist)
        {
            send_text_response(boost::beast::http::status::not_found, "text/plain", "stream not found\n", yield);
            return;
        }
        if (send_text_response(boost::beast::http::status::ok, "application/vnd.apple.mpegurl", *playlist, yield) && viewer->refresh() &&
            viewer->mark_streaming())
        {
            http_event::report_hls_output(event_state::streaming, viewer->stream_id(), viewer->stream_name(), "streaming");
        }
        return;
    }

    if (file == "init.mp4")
    {
        const auto init = hls::init_segment(stream_name, config_);
        if (!init)
        {
            send_text_response(boost::beast::http::status::not_found, "text/plain", "init segment not found\n", yield);
            return;
        }
        if (send_binary_response(boost::beast::http::status::ok, "video/mp4", *init, yield))
        {
            static_cast<void>(viewer->refresh());
        }
        return;
    }

    const bool transport_stream = file.ends_with(".ts");
    const bool fragmented_mp4 = file.ends_with(".m4s");
    const bool fmp4_mode = config_.http_video.codec == video_transcode_codec::av1;
    if ((!transport_stream && !fragmented_mp4) || (transport_stream && fmp4_mode) || (fragmented_mp4 && !fmp4_mode))
    {
        send_text_response(boost::beast::http::status::not_found, "text/plain", "not found\n", yield);
        return;
    }

    const auto suffix_size = transport_stream ? 3U : 4U;
    const std::string_view number(file.data(), file.size() - suffix_size);
    std::uint64_t sequence = 0;
    const auto [pointer, parse_error] = std::from_chars(number.data(), number.data() + number.size(), sequence);
    if (parse_error != std::errc{} || pointer != number.data() + number.size())
    {
        send_text_response(boost::beast::http::status::not_found, "text/plain", "not found\n", yield);
        return;
    }

    const auto segment = hls::segment(stream_name, sequence, config_);
    if (!segment)
    {
        send_text_response(boost::beast::http::status::not_found, "text/plain", "segment not found\n", yield);
        return;
    }
    if (send_binary_response(boost::beast::http::status::ok, fragmented_mp4 ? "video/mp4" : "video/mp2t", *segment, yield))
    {
        static_cast<void>(viewer->refresh());
    }
}

bool hls_http_session::send_text_response(
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
        return !error;
    }
    boost::beast::http::async_write(stream_, response, yield[error]);
    return !error;
}

bool hls_http_session::send_binary_response(boost::beast::http::status status,
                                            std::string_view content_type,
                                            std::vector<std::uint8_t> body,
                                            boost::asio::yield_context& yield)
{
    boost::beast::http::response<boost::beast::http::vector_body<std::uint8_t>> response(status, request_.version());
    response.set(boost::beast::http::field::server, "media_server");
    response.set(boost::beast::http::field::content_type, content_type);
    response.keep_alive(false);
    response.body() = std::move(body);
    response.prepare_payload();

    boost::system::error_code error;
    boost::beast::http::async_write(stream_, response, yield[error]);
    return !error;
}

void hls_http_session::shutdown()
{
    const auto self = shared_from_this();
    boost::asio::post(worker_.io(), [self]() { self->safe_shutdown(); });
}

void hls_http_session::safe_shutdown()
{
    if (closed_)
    {
        return;
    }
    closed_ = true;
    boost::system::error_code error;
    wait_timer_.cancel();
    stream_.socket().shutdown(boost::asio::ip::tcp::socket::shutdown_both, error);
    stream_.socket().close(error);
}

}    // namespace media_server
