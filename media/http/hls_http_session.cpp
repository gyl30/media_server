#include <chrono>
#include <utility>
#include <charconv>
#include <optional>

#include <boost/asio/post.hpp>

#include "media/hls/hls.h"
#include "media/hls/hls_segmenter.h"
#include "media/net/worker_context.h"
#include "media/hls/hls_play_session.h"
#include "media/http/hls_http_session.h"
#include "media/http/signaling_verify.h"
#include "media/http/http_target.h"
#include "media/core/stream_registry.h"

namespace media_server
{

hls_http_session::hls_http_session(worker_context& worker, boost::beast::tcp_stream stream, request_type request, const config& application_config)
    : worker_(worker), stream_(std::move(stream)), request_(std::move(request)), config_(application_config), wait_timer_(worker_.io())
{
}

void hls_http_session::startup()
{
    const auto self = shared_from_this();
    shutdown_subscription_ = worker_.subscribe_shutdown([self]() { self->shutdown(); });
    if (!shutdown_subscription_)
    {
        shutdown();
        return;
    }
    worker_.spawn([self](boost::asio::yield_context yield) { self->handle_request(yield); });
}

void hls_http_session::handle_request(boost::asio::yield_context yield)
{
    if (!stream_.socket().is_open())
    {
        return;
    }
    if (request_.method() != boost::beast::http::verb::get)
    {
        send_text_response(boost::beast::http::status::method_not_allowed, "text/plain", "method not allowed\n", false, "GET");
        return;
    }

    const std::string_view raw_target(request_.target().data(), request_.target().size());
    if (raw_target.find_first_of("%?#") != std::string_view::npos)
    {
        send_text_response(boost::beast::http::status::bad_request, "text/plain", "bad request target\n", false);
        return;
    }
    const auto target = parse_http_target(raw_target);
    if (!target)
    {
        send_text_response(boost::beast::http::status::bad_request, "text/plain", "bad request target\n", false);
        return;
    }
    std::vector<std::string> path;
    auto pathname = target->get_pathname().substr(1);
    for (;;)
    {
        const auto separator = pathname.find('/');
        path.emplace_back(pathname.substr(0, separator));
        if (separator == std::string_view::npos)
        {
            break;
        }
        pathname.remove_prefix(separator + 1);
    }

    if (path.size() != 5 || path[0] != "play" || path[1] != "hls")
    {
        send_text_response(boost::beast::http::status::not_found, "text/plain", "not found\n", false);
        return;
    }

    const auto& file = path[4];
    if (path[2] != "session")
    {
        const auto& stream_id = path[2];
        const auto& token = path[3];
        if (file != "index.m3u8" || !valid_stream_id(stream_id) || !valid_stream_token(token))
        {
            send_text_response(boost::beast::http::status::bad_request, "text/plain", "invalid playback target\n", false);
            return;
        }
        const auto source = stream_registry::instance().find(stream_id);
        if (!source)
        {
            send_text_response(boost::beast::http::status::not_found, "text/plain", "stream not found\n", false);
            return;
        }
        const auto segmenter = hls::get_or_create(source);
        if (!segmenter)
        {
            send_text_response(boost::beast::http::status::not_found, "text/plain", "stream not found\n", false);
            return;
        }
        if (!verify_stream(config_, token, "play", stream_id, yield))
        {
            send_text_response(boost::beast::http::status::forbidden, "text/plain", "playback denied\n", false);
            return;
        }
        if (!stream_.socket().is_open())
        {
            return;
        }
        const auto viewer = hls_play_session::create(worker_, segmenter);
        send_redirect("/play/hls/session/" + viewer->secret() + "/index.m3u8");
        return;
    }

    std::optional<std::uint64_t> segment_sequence;
    if (file != "index.m3u8")
    {
        if (!file.ends_with(".ts"))
        {
            send_text_response(boost::beast::http::status::not_found, "text/plain", "not found\n", false);
            return;
        }
        const std::string_view number(file.data(), file.size() - 3U);
        std::uint64_t sequence{};
        const auto [pointer, parse_error] = std::from_chars(number.data(), number.data() + number.size(), sequence);
        if (parse_error != std::errc{} || pointer != number.data() + number.size())
        {
            send_text_response(boost::beast::http::status::not_found, "text/plain", "not found\n", false);
            return;
        }
        segment_sequence = sequence;
    }

    const auto viewer = hls_play_session::find(path[3]);
    if (!viewer)
    {
        send_text_response(boost::beast::http::status::forbidden, "text/plain", "invalid hls session\n", false);
        return;
    }
    const auto segmenter = viewer->segmenter();
    if (!segment_sequence)
    {
        playlist_deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        wait_for_playlist(segmenter);
        return;
    }

    const auto segment = segmenter->segment_buffer(*segment_sequence);
    if (!segment)
    {
        send_text_response(boost::beast::http::status::not_found, "text/plain", "segment not found\n", false);
        return;
    }
    send_binary_response(boost::beast::http::status::ok, "video/mp2t", segment, request_.keep_alive());
}

void hls_http_session::wait_for_playlist(std::shared_ptr<hls_segmenter> segmenter)
{
    if (!stream_.socket().is_open())
    {
        return;
    }
    if (segmenter->has_segments())
    {
        const auto playlist = segmenter->playlist();
        send_text_response(boost::beast::http::status::ok, "application/vnd.apple.mpegurl", playlist, request_.keep_alive());
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
        [self, segmenter = std::move(segmenter)](const boost::system::error_code& error) mutable
        {
            if (error)
            {
                return;
            }
            self->wait_for_playlist(std::move(segmenter));
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
                                          std::string_view allow)
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
            [self, response, serializer, keep_alive](const boost::system::error_code& error, std::size_t)
            { self->response_completed(error, keep_alive); });
        return;
    }
    boost::beast::http::async_write(
        stream_,
        *response,
        [self, response, keep_alive](const boost::system::error_code& error, std::size_t) { self->response_completed(error, keep_alive); });
}

void hls_http_session::send_binary_response(boost::beast::http::status status,
                                            std::string_view content_type,
                                            std::shared_ptr<const std::vector<std::uint8_t>> body,
                                            bool keep_alive)
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
        [self, response, body = std::move(body), keep_alive](const boost::system::error_code& error, std::size_t)
        { self->response_completed(error, keep_alive); });
}

void hls_http_session::response_completed(const boost::system::error_code& error, bool keep_alive)
{
    if (error || !stream_.socket().is_open() || !keep_alive)
    {
        shutdown();
        return;
    }
    read_request();
}

void hls_http_session::read_request()
{
    if (!stream_.socket().is_open())
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
                                   {
                                       if (error)
                                       {
                                           self->shutdown();
                                           return;
                                       }
                                       self->worker_.spawn([self](boost::asio::yield_context yield) { self->handle_request(yield); });
                                   });
}

void hls_http_session::shutdown()
{
    const auto self = shared_from_this();
    boost::asio::post(worker_.io(), [self]() { self->safe_shutdown(); });
}

void hls_http_session::safe_shutdown()
{
    shutdown_subscription_.reset();
    boost::system::error_code error;
    wait_timer_.cancel();
    stream_.socket().shutdown(boost::asio::ip::tcp::socket::shutdown_both, error);
    stream_.socket().close(error);
}

}    // namespace media_server
