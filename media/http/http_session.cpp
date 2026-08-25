#include <chrono>
#include <utility>
#include <charconv>

#include <boost/asio/post.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/http/chunk_encode.hpp>
#include <boost/url/parse.hpp>

#include "media/http/gb28181_http.h"
#include "media/http/gb28181_json.h"
#include "media/http/http_session.h"
#include "media/hls/hls.h"
#include "media/core/stream_registry.h"
#include "media/webrtc/whep.h"
#include "media/net/io_context_pool.h"

namespace media_server
{
namespace
{

constexpr int whep_retry_after_seconds = 1;

}    // namespace

http_session::http_session(boost::asio::ip::tcp::socket socket,
                           io_context_pool& workers,
                           const config& config)
    : stream_(std::move(socket)),
      workers_(workers),
      config_(config),
      hls_wait_timer_(stream_.get_executor())
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

    const auto segments = path_segments(*parsed);
    if (segments.empty())
    {
        send_text_response(boost::beast::http::status::not_found, "text/plain", "not found\n");
        return;
    }

    if (segments.front() == "whep")
    {
        handle_whep(segments);
        return;
    }
    if (segments.front() == "gb28181")
    {
        handle_gb28181(*parsed, segments);
        return;
    }

    if (request_.method() != boost::beast::http::verb::get)
    {
        send_text_response(boost::beast::http::status::method_not_allowed, "text/plain", "method not allowed\n", "GET");
        return;
    }

    if (segments.front() == "hls")
    {
        handle_hls(*parsed);
        return;
    }

    if (segments.back().ends_with(".flv"))
    {
        handle_flv(*parsed);
        return;
    }

    send_text_response(boost::beast::http::status::not_found, "text/plain", "not found\n");
}

void http_session::handle_whep(const std::vector<std::string>& segments)
{
    const bool session_resource = segments.size() == 3 && segments[1] == "session";
    const bool endpoint_resource = segments.size() >= 2 && segments[1] != "session";
    if (!session_resource && !endpoint_resource)
    {
        send_whep_error_response(boost::beast::http::status::not_found, "not found\n");
        return;
    }

    if (request_.method() == boost::beast::http::verb::options)
    {
        send_whep_options_response(session_resource);
        return;
    }
    if (request_.method() == boost::beast::http::verb::get || request_.method() == boost::beast::http::verb::head)
    {
        if (session_resource && !whep::contains(segments[2]))
        {
            send_whep_empty_response(boost::beast::http::status::not_found);
            return;
        }
        if (endpoint_resource)
        {
            send_whep_empty_response(boost::beast::http::status::ok, "application/sdp");
        }
        else
        {
            send_whep_empty_response(boost::beast::http::status::no_content);
        }
        return;
    }
    if (request_.method() == boost::beast::http::verb::post && endpoint_resource)
    {
        handle_whep_post(segments);
        return;
    }
    if (request_.method() == boost::beast::http::verb::delete_ && session_resource)
    {
        handle_whep_delete(segments);
        return;
    }

    const std::string_view allow = session_resource ? "GET, HEAD, DELETE, OPTIONS" : "GET, HEAD, POST, OPTIONS";
    // 本实现只支持一次完整 SDP POST/answer，不支持 PATCH/Trickle ICE。
    send_whep_error_response(boost::beast::http::status::method_not_allowed, "method not allowed\n", 0, allow);
}

void http_session::handle_whep_post(const std::vector<std::string>& segments)
{
    const auto content_type = request_[boost::beast::http::field::content_type];
    if (!boost::beast::iequals(content_type, "application/sdp"))
    {
        send_whep_error_response(boost::beast::http::status::unsupported_media_type, "content type must be application/sdp\n");
        return;
    }

    const auto stream_name = join_segments(segments, 1, segments.size());
    auto result = whep::create(stream_.get_executor(), stream_name, request_.body(), config_);
    switch (result.error)
    {
        case whep::create_error::none:
            send_whep_response(std::move(result.session_id), std::move(result.answer_sdp));
            return;
        case whep::create_error::stream_not_found:
            send_whep_error_response(boost::beast::http::status::conflict, "stream not found\n", whep_retry_after_seconds);
            return;
        case whep::create_error::invalid_offer:
            send_whep_error_response(boost::beast::http::status::bad_request, "invalid or unsupported sdp offer\n");
            return;
        case whep::create_error::internal_error:
            send_whep_error_response(boost::beast::http::status::internal_server_error, "whep session create failed\n");
            return;
    }
}

void http_session::handle_whep_delete(const std::vector<std::string>& segments)
{
    if (!whep::remove(segments[2]))
    {
        send_whep_error_response(boost::beast::http::status::not_found, "whep session not found\n");
        return;
    }
    send_whep_empty_response(boost::beast::http::status::no_content);
}

void http_session::handle_gb28181(const boost::urls::url_view& target, const std::vector<std::string>& segments)
{
    const bool input_create = segments.size() == 3 && segments[1] == "input" && segments[2] == "create";
    const bool input_delete = segments.size() == 3 && segments[1] == "input" && segments[2] == "delete";
    const bool output_create = segments.size() == 3 && segments[1] == "output" && segments[2] == "create";
    const bool output_delete = segments.size() == 3 && segments[1] == "output" && segments[2] == "delete";
    if (!input_create && !input_delete && !output_create && !output_delete)
    {
        write_response(make_gb28181_error_response(request_, boost::beast::http::status::not_found, "not_found"));
        return;
    }
    if (!target.params().empty())
    {
        write_response(make_gb28181_error_response(request_, boost::beast::http::status::bad_request, "invalid_request"));
        return;
    }
    if (request_.method() != boost::beast::http::verb::post)
    {
        write_response(make_gb28181_error_response(request_, boost::beast::http::status::method_not_allowed, "method_not_allowed", "POST"));
        return;
    }
    const auto content_type = request_[boost::beast::http::field::content_type];
    if (!boost::beast::iequals(content_type, "application/json"))
    {
        write_response(make_gb28181_error_response(request_, boost::beast::http::status::unsupported_media_type, "unsupported_media_type"));
        return;
    }
    if (input_create)
    {
        handle_gb28181_input_create();
    }
    else if (input_delete)
    {
        handle_gb28181_input_delete();
    }
    else if (output_create)
    {
        handle_gb28181_output_create();
    }
    else
    {
        handle_gb28181_output_delete();
    }
}

void http_session::handle_gb28181_input_create()
{
    auto config = parse_gb28181_input_config(request_.body());
    if (!config)
    {
        write_response(make_gb28181_error_response(request_, boost::beast::http::status::bad_request, "invalid_request"));
        return;
    }
    write_response(media_server::handle_gb28181_input_create(request_, workers_.next(), std::move(*config)));
}

void http_session::handle_gb28181_input_delete()
{
    const auto stream_name = parse_gb28181_input_delete(request_.body());
    if (!stream_name)
    {
        write_response(make_gb28181_error_response(request_, boost::beast::http::status::bad_request, "invalid_request"));
        return;
    }
    write_response(media_server::handle_gb28181_input_delete(request_, *stream_name));
}

void http_session::handle_gb28181_output_create()
{
    auto config = parse_gb28181_output_config(request_.body());
    if (!config)
    {
        write_response(make_gb28181_error_response(request_, boost::beast::http::status::bad_request, "invalid_request"));
        return;
    }
    write_response(media_server::handle_gb28181_output_create(request_, workers_.next(), std::move(*config)));
}

void http_session::handle_gb28181_output_delete()
{
    const auto identity = parse_gb28181_output_delete(request_.body());
    if (!identity)
    {
        write_response(make_gb28181_error_response(request_, boost::beast::http::status::bad_request, "invalid_request"));
        return;
    }
    write_response(media_server::handle_gb28181_output_delete(request_, identity->first, identity->second));
}

void http_session::handle_flv(const boost::urls::url_view& target)
{
    auto segments = path_segments(target);
    if (segments.empty() || !segments.back().ends_with(".flv"))
    {
        send_text_response(boost::beast::http::status::not_found, "text/plain", "not found\n");
        return;
    }

    segments.back().resize(segments.back().size() - 4);
    const auto stream_name = join_segments(segments, 0, segments.size());
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

void http_session::handle_hls(const boost::urls::url_view& target)
{
    const auto segments = path_segments(target);
    if (segments.size() < 3 || segments.front() != "hls")
    {
        send_text_response(boost::beast::http::status::not_found, "text/plain", "not found\n");
        return;
    }

    const auto& file = segments.back();
    const auto stream_name = join_segments(segments, 1, segments.size() - 1);
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
            wait_hls_playlist(stream_name);
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

void http_session::wait_hls_playlist(std::string stream_name)
{
    hls_wait_stream_name_ = std::move(stream_name);
    hls_wait_deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    check_hls_playlist();
}

void http_session::check_hls_playlist()
{
    if (closed_)
    {
        return;
    }

    const auto count = hls::segment_count(hls_wait_stream_name_, config_);
    if (!count)
    {
        send_text_response(boost::beast::http::status::not_found, "text/plain", "stream not found\n");
        return;
    }

    if (*count > 0)
    {
        const auto playlist = hls::playlist(hls_wait_stream_name_, config_);
        if (!playlist)
        {
            send_text_response(boost::beast::http::status::not_found, "text/plain", "stream not found\n");
            return;
        }
        send_text_response(boost::beast::http::status::ok, "application/vnd.apple.mpegurl", *playlist);
        return;
    }

    if (std::chrono::steady_clock::now() >= hls_wait_deadline_)
    {
        send_text_response(boost::beast::http::status::service_unavailable, "text/plain", "hls playlist not ready\n");
        return;
    }

    hls_wait_timer_.expires_after(std::chrono::milliseconds(100));
    const auto self = shared_from_this();
    hls_wait_timer_.async_wait(
        [self](boost::system::error_code error)
        {
            if (!error)
            {
                self->check_hls_playlist();
            }
        });
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

void http_session::send_whep_error_response(boost::beast::http::status status, std::string body, int retry_after_seconds, std::string_view allow)
{
    auto response = std::make_shared<boost::beast::http::response<boost::beast::http::string_body>>(status, request_.version());
    response->set(boost::beast::http::field::server, "media_server");
    response->set(boost::beast::http::field::content_type, "text/plain");
    response->set(boost::beast::http::field::access_control_allow_origin, "*");
    if (retry_after_seconds > 0)
    {
        response->set(boost::beast::http::field::retry_after, std::to_string(retry_after_seconds));
    }
    if (!allow.empty())
    {
        response->set(boost::beast::http::field::allow, allow);
    }
    response->keep_alive(false);
    response->body() = std::move(body);
    response->prepare_payload();

    write_string_response(std::move(response));
}

void http_session::send_whep_options_response(bool session_resource)
{
    auto response =
        std::make_shared<boost::beast::http::response<boost::beast::http::empty_body>>(boost::beast::http::status::ok, request_.version());
    response->set(boost::beast::http::field::server, "media_server");
    response->set(boost::beast::http::field::access_control_allow_origin, "*");
    response->set(boost::beast::http::field::access_control_allow_methods,
                  session_resource ? "GET, HEAD, DELETE, OPTIONS" : "GET, HEAD, POST, OPTIONS");
    response->set(boost::beast::http::field::access_control_allow_headers, "Content-Type");
    if (!session_resource)
    {
        response->set("Accept-Post", "application/sdp");
    }
    response->keep_alive(false);
    response->content_length(0);

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

void http_session::send_whep_response(std::string session_id, std::string answer_sdp)
{
    auto response =
        std::make_shared<boost::beast::http::response<boost::beast::http::string_body>>(boost::beast::http::status::created, request_.version());
    response->set(boost::beast::http::field::server, "media_server");
    response->set(boost::beast::http::field::content_type, "application/sdp");
    response->set(boost::beast::http::field::location, "/whep/session/" + session_id);
    response->set(boost::beast::http::field::cache_control, "no-store");
    response->set(boost::beast::http::field::access_control_allow_origin, "*");
    response->set(boost::beast::http::field::access_control_expose_headers, "Location");
    response->keep_alive(false);
    response->body() = std::move(answer_sdp);
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

void http_session::send_whep_empty_response(boost::beast::http::status status, std::string_view content_type)
{
    auto response = std::make_shared<boost::beast::http::response<boost::beast::http::empty_body>>(status, request_.version());
    response->set(boost::beast::http::field::server, "media_server");
    response->set(boost::beast::http::field::cache_control, "no-store");
    response->set(boost::beast::http::field::access_control_allow_origin, "*");
    if (!content_type.empty())
    {
        response->set(boost::beast::http::field::content_type, content_type);
        response->content_length(0);
    }
    response->keep_alive(false);

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

void http_session::send_binary_response(boost::beast::http::status status, std::string_view content_type, std::vector<std::uint8_t> body)
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

void http_session::startup_flv(std::shared_ptr<media_stream> media_stream)
{
    if (closed_)
    {
        return;
    }

    const auto weak = weak_from_this();
    flv_output_ = std::make_shared<http_flv_output>(
        [weak](std::uint64_t generation, std::vector<std::uint8_t> data, bool bootstrap)
        {
            if (const auto self = weak.lock())
            {
                self->enqueue_flv(generation, std::move(data), bootstrap);
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

    flv_reader_ = media_stream->add_reader(flv_output_, stream_.get_executor());
    read_flv_client();
}

void http_session::read_flv_client()
{
    if (closed_)
    {
        return;
    }

    const auto self = shared_from_this();
    stream_.async_read_some(boost::asio::buffer(flv_read_buffer_),
                            [self](boost::system::error_code error, std::size_t bytes)
                            {
                                static_cast<void>(bytes);
                                if (error)
                                {
                                    self->shutdown();
                                    return;
                                }
                                self->read_flv_client();
                            });
}

void http_session::enqueue_flv(std::uint64_t generation, std::vector<std::uint8_t> data, bool bootstrap)
{
    if (closed_)
    {
        return;
    }

    if (flv_write_in_progress_)
    {
        if (!bootstrap)
        {
            shutdown();
            return;
        }
        pending_flv_generation_ = generation;
        pending_flv_bootstrap_ = std::move(data);
        pending_flv_bootstrap_ready_ = true;
        return;
    }

    if (data.empty())
    {
        flv_output_->write_complete(generation);
        return;
    }
    write_flv(generation, std::move(data));
}

void http_session::write_flv(std::uint64_t generation, std::vector<std::uint8_t> data)
{
    flv_write_in_progress_ = true;
    const auto buffer = std::make_shared<std::vector<std::uint8_t>>(std::move(data));
    const auto chunk = std::make_shared<decltype(boost::beast::http::make_chunk(boost::asio::buffer(*buffer)))>(
        boost::beast::http::make_chunk(boost::asio::buffer(*buffer)));
    const auto self = shared_from_this();
    boost::asio::async_write(stream_,
                             *chunk,
                             [self, buffer, chunk, generation](boost::system::error_code error, std::size_t bytes)
                             {
                                 static_cast<void>(buffer);
                                 static_cast<void>(bytes);
                                 self->on_flv_write(generation, error);
                             });
}

void http_session::on_flv_write(std::uint64_t generation, boost::system::error_code error)
{
    if (closed_)
    {
        return;
    }
    flv_write_in_progress_ = false;
    if (error)
    {
        shutdown();
        return;
    }
    if (pending_flv_bootstrap_ready_)
    {
        const auto pending_generation = pending_flv_generation_;
        auto pending = std::move(pending_flv_bootstrap_);
        pending_flv_bootstrap_ready_ = false;
        if (pending.empty())
        {
            flv_output_->write_complete(pending_generation);
        }
        else
        {
            write_flv(pending_generation, std::move(pending));
        }
        return;
    }
    flv_output_->write_complete(generation);
}

void http_session::detach_flv()
{
    flv_reader_.remove();
    flv_reader_ = {};
    flv_output_.reset();
    pending_flv_bootstrap_.clear();
    pending_flv_bootstrap_ready_ = false;
    flv_write_in_progress_ = false;
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
    detach_flv();
    boost::system::error_code error;
    hls_wait_timer_.cancel();
    stream_.socket().shutdown(boost::asio::ip::tcp::socket::shutdown_both, error);
    stream_.socket().close(error);
}

std::vector<std::string> http_session::path_segments(const boost::urls::url_view& target)
{
    std::vector<std::string> result;
    for (const auto segment : target.segments())
    {
        result.emplace_back(segment);
    }
    return result;
}

std::string http_session::join_segments(const std::vector<std::string>& segments, std::size_t begin, std::size_t end)
{
    if (begin >= end || end > segments.size())
    {
        return {};
    }

    std::string result;
    for (std::size_t index = begin; index < end; ++index)
    {
        if (!result.empty())
        {
            result.push_back('/');
        }
        result.append(segments[index]);
    }
    return result;
}
}    // namespace media_server
