#include <chrono>
#include <utility>
#include <charconv>

#include <boost/asio/post.hpp>
#include <boost/url/parse.hpp>

#include "media/hls/hls.h"
#include "media/http/hls_http_session.h"

namespace media_server
{

hls_http_session::hls_http_session(boost::beast::tcp_stream stream, request_type request, const config& config)
    : stream_(std::move(stream)), request_(std::move(request)), config_(config), wait_timer_(stream_.get_executor())
{
}

void hls_http_session::startup() { handle_request(); }

void hls_http_session::handle_request()
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

    if (path.size() < 4 || path[0] != "play" || path[1] != "hls")
    {
        send_text_response(boost::beast::http::status::not_found, "text/plain", "not found\n");
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
        send_text_response(boost::beast::http::status::not_found, "text/plain", "not found\n");
        return;
    }

    if (file == "index.m3u8")
    {
        const auto count = hls::segment_count(stream_name, config_);
        if (!count)
        {
            send_text_response(boost::beast::http::status::not_found, "text/plain", "stream not found\n");
            return;
        }
        if (*count == 0)
        {
            wait_playlist(std::move(stream_name));
            return;
        }

        const auto playlist = hls::playlist(stream_name, config_);
        if (!playlist)
        {
            send_text_response(boost::beast::http::status::not_found, "text/plain", "stream not found\n");
            return;
        }
        send_text_response(boost::beast::http::status::ok, "application/vnd.apple.mpegurl", *playlist);
        return;
    }

    if (file == "init.mp4")
    {
        const auto init = hls::init_segment(stream_name, config_);
        if (!init)
        {
            send_text_response(boost::beast::http::status::not_found, "text/plain", "init segment not found\n");
            return;
        }
        send_binary_response(boost::beast::http::status::ok, "video/mp4", *init);
        return;
    }

    const bool transport_stream = file.ends_with(".ts");
    const bool fragmented_mp4 = file.ends_with(".m4s");
    const bool fmp4_mode = config_.http_video.codec == output_video_codec::av1;
    if ((!transport_stream && !fragmented_mp4) || (transport_stream && fmp4_mode) || (fragmented_mp4 && !fmp4_mode))
    {
        send_text_response(boost::beast::http::status::not_found, "text/plain", "not found\n");
        return;
    }

    const auto suffix_size = transport_stream ? 3U : 4U;
    const std::string_view number(file.data(), file.size() - suffix_size);
    std::uint64_t sequence = 0;
    const auto [pointer, parse_error] = std::from_chars(number.data(), number.data() + number.size(), sequence);
    if (parse_error != std::errc{} || pointer != number.data() + number.size())
    {
        send_text_response(boost::beast::http::status::not_found, "text/plain", "not found\n");
        return;
    }

    const auto segment = hls::segment(stream_name, sequence, config_);
    if (!segment)
    {
        send_text_response(boost::beast::http::status::not_found, "text/plain", "segment not found\n");
        return;
    }
    send_binary_response(boost::beast::http::status::ok, fragmented_mp4 ? "video/mp4" : "video/mp2t", *segment);
}

void hls_http_session::wait_playlist(std::string stream_name)
{
    wait_stream_name_ = std::move(stream_name);
    wait_deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    check_playlist();
}

void hls_http_session::check_playlist()
{
    if (closed_)
    {
        return;
    }

    const auto count = hls::segment_count(wait_stream_name_, config_);
    if (!count)
    {
        send_text_response(boost::beast::http::status::not_found, "text/plain", "stream not found\n");
        return;
    }

    if (*count > 0)
    {
        const auto playlist = hls::playlist(wait_stream_name_, config_);
        if (!playlist)
        {
            send_text_response(boost::beast::http::status::not_found, "text/plain", "stream not found\n");
            return;
        }
        send_text_response(boost::beast::http::status::ok, "application/vnd.apple.mpegurl", *playlist);
        return;
    }

    if (std::chrono::steady_clock::now() >= wait_deadline_)
    {
        send_text_response(boost::beast::http::status::service_unavailable, "text/plain", "hls playlist not ready\n");
        return;
    }

    wait_timer_.expires_after(std::chrono::milliseconds(100));
    const auto self = shared_from_this();
    wait_timer_.async_wait(
        [self](boost::system::error_code error)
        {
            if (!error)
            {
                self->check_playlist();
            }
        });
}

void hls_http_session::write_string_response(std::shared_ptr<boost::beast::http::response<boost::beast::http::string_body>> response)
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

void hls_http_session::send_text_response(boost::beast::http::status status, std::string_view content_type, std::string body, std::string_view allow)
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

void hls_http_session::send_binary_response(boost::beast::http::status status, std::string_view content_type, std::vector<std::uint8_t> body)
{
    auto response = std::make_shared<boost::beast::http::response<boost::beast::http::vector_body<std::uint8_t>>>(status, request_.version());
    response->set(boost::beast::http::field::server, "media_server");
    response->set(boost::beast::http::field::content_type, content_type);
    response->keep_alive(false);
    response->body() = std::move(body);
    response->prepare_payload();

    const auto self = shared_from_this();
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

void hls_http_session::shutdown()
{
    const auto self = shared_from_this();
    boost::asio::post(stream_.get_executor(), [self]() { self->safe_shutdown(); });
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
