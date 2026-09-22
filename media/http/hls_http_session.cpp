#include <chrono>
#include <utility>
#include <charconv>
#include <optional>

#include <boost/asio/post.hpp>
#include <boost/url/parse.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/detached.hpp>

#include "media/hls/hls.h"
#include "media/core/stream_id.h"
#include "media/http/http_event.h"
#include "media/hls/hls_segmenter.h"
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
    boost::asio::post(worker_.io(), [self]() { self->handle_request(); });
}

void hls_http_session::handle_request()
{
    if (closed_)
    {
        return;
    }
    if (request_.method() != boost::beast::http::verb::get)
    {
        send_text_response(boost::beast::http::status::method_not_allowed, "text/plain", "method not allowed\n", false, "GET");
        return;
    }

    const auto parsed = boost::urls::parse_origin_form(request_.target());
    if (!parsed)
    {
        send_text_response(boost::beast::http::status::bad_request, "text/plain", "bad request target\n", false);
        return;
    }
    const auto target = *parsed;
    std::vector<std::string> path;
    for (const auto segment : target.segments())
    {
        path.emplace_back(segment);
    }

    if (path.size() < 4 || path[0] != "play" || path[1] != "hls")
    {
        send_text_response(boost::beast::http::status::not_found, "text/plain", "not found\n", false);
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
        send_text_response(boost::beast::http::status::not_found, "text/plain", "not found\n", false);
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
                send_text_response(boost::beast::http::status::bad_request, "text/plain", "invalid stream id\n", false);
                return;
            }
            stream_id = parameter.value;
        }
        else if (parameter.key == "session")
        {
            if (secret || !parameter.has_value)
            {
                send_text_response(boost::beast::http::status::forbidden, "text/plain", "invalid hls session\n", false);
                return;
            }
            secret = parameter.value;
        }
    }

    if (stream_id)
    {
        if (file != "index.m3u8" || secret || !valid_stream_id(*stream_id))
        {
            send_text_response(boost::beast::http::status::bad_request, "text/plain", "invalid stream id\n", false);
            return;
        }
        claim_play(std::move(*stream_id), std::move(stream_name), std::string(target.encoded_path()));
        return;
    }

    if (!secret)
    {
        send_text_response(boost::beast::http::status::forbidden, "text/plain", "hls session required\n", false);
        return;
    }
    const auto viewer = hls_play_session::find(*secret, stream_name);
    if (!viewer)
    {
        send_text_response(boost::beast::http::status::forbidden, "text/plain", "invalid hls session\n", false);
        return;
    }
    const auto segmenter = viewer->segmenter();

    if (file == "index.m3u8")
    {
        if (!segmenter)
        {
            send_text_response(boost::beast::http::status::not_found, "text/plain", "stream not found\n", false);
            return;
        }
        playlist_deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        wait_for_playlist(viewer, segmenter);
        return;
    }

    if (file == "init.mp4")
    {
        const auto init = segmenter ? segmenter->init_segment() : std::nullopt;
        if (!init)
        {
            send_text_response(boost::beast::http::status::not_found, "text/plain", "init segment not found\n", false);
            return;
        }
        send_binary_response(
            boost::beast::http::status::ok, "video/mp4", std::make_shared<const std::vector<std::uint8_t>>(*init), request_.keep_alive(), viewer);
        return;
    }

    const bool transport_stream = file.ends_with(".ts");
    const bool fragmented_mp4 = file.ends_with(".m4s");
    const bool fmp4_mode = config_.http_video.codec == video_transcode_codec::av1;
    if ((!transport_stream && !fragmented_mp4) || (transport_stream && fmp4_mode) || (fragmented_mp4 && !fmp4_mode))
    {
        send_text_response(boost::beast::http::status::not_found, "text/plain", "not found\n", false);
        return;
    }

    const auto suffix_size = transport_stream ? 3U : 4U;
    const std::string_view number(file.data(), file.size() - suffix_size);
    std::uint64_t sequence = 0;
    const auto [pointer, parse_error] = std::from_chars(number.data(), number.data() + number.size(), sequence);
    if (parse_error != std::errc{} || pointer != number.data() + number.size())
    {
        send_text_response(boost::beast::http::status::not_found, "text/plain", "not found\n", false);
        return;
    }

    const auto segment = segmenter ? segmenter->segment_buffer(sequence) : std::shared_ptr<const std::vector<std::uint8_t>>{};
    if (!segment)
    {
        send_text_response(boost::beast::http::status::not_found, "text/plain", "segment not found\n", false);
        return;
    }
    send_binary_response(boost::beast::http::status::ok, fragmented_mp4 ? "video/mp4" : "video/mp2t", segment, request_.keep_alive(), viewer);
}

void hls_http_session::claim_play(std::string stream_id, std::string stream_name, std::string redirect_path)
{
    const auto self = shared_from_this();
    boost::asio::spawn(
        worker_.io(),
        [self, stream_id = std::move(stream_id), stream_name = std::move(stream_name), redirect_path = std::move(redirect_path)](
            boost::asio::yield_context yield) mutable
        {
            auto result = signaling_client::instance().claim_play(stream_id, "hls", stream_name, yield);
            self->handle_claim(std::move(stream_id), std::move(stream_name), std::move(redirect_path), std::move(result));
        },
        boost::asio::detached);
}

void hls_http_session::handle_claim(std::string stream_id, std::string stream_name, std::string redirect_path, signaling_request_result result)
{
    if (closed_)
    {
        return;
    }
    if (result.kind != signaling_result_kind::accepted)
    {
        const auto status =
            result.kind == signaling_result_kind::rejected ? boost::beast::http::status::forbidden : boost::beast::http::status::service_unavailable;
        send_text_response(status, "text/plain", "play claim failed\n", false);
        return;
    }

    const auto segmenter = hls::get_or_create(stream_name, config_);
    const auto viewer = hls_play_session::create(worker_, std::move(stream_id), std::move(stream_name), segmenter);
    http_event::report_hls_output(event_state::starting, viewer->stream_id(), viewer->stream_name(), "play");
    send_redirect(std::move(redirect_path) + "?session=" + viewer->secret());
}

void hls_http_session::wait_for_playlist(std::shared_ptr<hls_play_session> viewer, std::shared_ptr<hls_segmenter> segmenter)
{
    if (closed_)
    {
        return;
    }
    if (segmenter->segment_count() != 0U)
    {
        const auto playlist = segmenter->playlist(".", "session=" + viewer->secret());
        send_text_response(boost::beast::http::status::ok, "application/vnd.apple.mpegurl", playlist, request_.keep_alive(), {}, viewer, true);
        return;
    }
    if (std::chrono::steady_clock::now() >= playlist_deadline_)
    {
        send_text_response(boost::beast::http::status::service_unavailable, "text/plain", "hls playlist not ready\n", false);
        return;
    }

    wait_timer_.expires_after(std::chrono::milliseconds(100));
    const auto self = shared_from_this();
    wait_timer_.async_wait(
        [self, viewer = std::move(viewer), segmenter = std::move(segmenter)](const boost::system::error_code& error) mutable
        {
            if (error || self->closed_)
            {
                return;
            }
            self->wait_for_playlist(std::move(viewer), std::move(segmenter));
        });
}

void hls_http_session::send_redirect(std::string location)
{
    auto response = std::make_shared<boost::beast::http::response<boost::beast::http::empty_body>>(boost::beast::http::status::temporary_redirect,
                                                                                                   request_.version());
    response->set(boost::beast::http::field::server, "media_server");
    response->set(boost::beast::http::field::location, std::move(location));
    response->keep_alive(false);

    stream_.expires_after(std::chrono::seconds(30));
    const auto self = shared_from_this();
    boost::beast::http::async_write(
        stream_, *response, [self, response](const boost::system::error_code& error, std::size_t) { self->response_completed(error, false); });
}

void hls_http_session::send_text_response(boost::beast::http::status status,
                                          std::string_view content_type,
                                          std::string body,
                                          bool keep_alive,
                                          std::string_view allow,
                                          std::shared_ptr<hls_play_session> viewer,
                                          bool mark_streaming)
{
    auto response = std::make_shared<boost::beast::http::response<boost::beast::http::string_body>>(status, request_.version());
    response->set(boost::beast::http::field::server, "media_server");
    response->set(boost::beast::http::field::content_type, content_type);
    if (!allow.empty())
    {
        response->set(boost::beast::http::field::allow, allow);
    }
    response->keep_alive(keep_alive);
    response->body() = std::move(body);
    response->prepare_payload();

    stream_.expires_after(std::chrono::seconds(30));
    const auto self = shared_from_this();
    if (request_.method() == boost::beast::http::verb::head)
    {
        auto serializer = std::make_shared<boost::beast::http::response_serializer<boost::beast::http::string_body>>(*response);
        boost::beast::http::async_write_header(
            stream_,
            *serializer,
            [self, response, serializer, keep_alive, viewer = std::move(viewer), mark_streaming](const boost::system::error_code& error, std::size_t)
            { self->response_completed(error, keep_alive, std::move(viewer), mark_streaming); });
        return;
    }
    boost::beast::http::async_write(
        stream_,
        *response,
        [self, response, keep_alive, viewer = std::move(viewer), mark_streaming](const boost::system::error_code& error, std::size_t)
        { self->response_completed(error, keep_alive, std::move(viewer), mark_streaming); });
}

void hls_http_session::send_binary_response(boost::beast::http::status status,
                                            std::string_view content_type,
                                            std::shared_ptr<const std::vector<std::uint8_t>> body,
                                            bool keep_alive,
                                            std::shared_ptr<hls_play_session> viewer)
{
    auto response = std::make_shared<boost::beast::http::response<boost::beast::http::buffer_body>>(status, request_.version());
    response->set(boost::beast::http::field::server, "media_server");
    response->set(boost::beast::http::field::content_type, content_type);
    response->keep_alive(keep_alive);
    response->body().data = const_cast<std::uint8_t*>(body->data());
    response->body().size = body->size();
    response->body().more = false;
    response->content_length(body->size());

    stream_.expires_after(std::chrono::seconds(30));
    const auto self = shared_from_this();
    boost::beast::http::async_write(
        stream_,
        *response,
        [self, response, body = std::move(body), keep_alive, viewer = std::move(viewer)](const boost::system::error_code& error, std::size_t)
        { self->response_completed(error, keep_alive, std::move(viewer)); });
}

void hls_http_session::response_completed(const boost::system::error_code& error,
                                          bool keep_alive,
                                          std::shared_ptr<hls_play_session> viewer,
                                          bool mark_streaming)
{
    if (!error && viewer && viewer->refresh() && mark_streaming && viewer->mark_streaming())
    {
        http_event::report_hls_output(event_state::streaming, viewer->stream_id(), viewer->stream_name(), "streaming");
    }
    if (error || closed_ || !keep_alive)
    {
        shutdown();
        return;
    }
    read_request();
}

void hls_http_session::read_request()
{
    if (closed_)
    {
        return;
    }
    request_ = {};
    stream_.expires_after(std::chrono::seconds(30));
    const auto self = shared_from_this();
    boost::beast::http::async_read(stream_,
                                   buffer_,
                                   request_,
                                   [self](const boost::system::error_code& error, std::size_t)
                                   { error ? self->shutdown() : self->handle_request(); });
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
