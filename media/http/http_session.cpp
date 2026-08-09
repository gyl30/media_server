#include "media/http/http_session.h"

#include "media/core/log.h"

#include <boost/asio/write.hpp>
#include <boost/beast/http/chunk_encode.hpp>
#include <boost/url/parse.hpp>

#include <charconv>
#include <chrono>
#include <utility>

namespace media_server
{
namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
namespace urls = boost::urls;

struct http_session::flv_chunk
{
    using chunk_type = decltype(http::make_chunk(net::buffer(std::declval<const std::vector<std::uint8_t>&>())));

    explicit flv_chunk(std::span<const std::uint8_t> value) : data(value.begin(), value.end()), chunk(http::make_chunk(net::buffer(static_cast<const std::vector<std::uint8_t>&>(data))))
    {
    }

    std::vector<std::uint8_t> data;
    chunk_type chunk;
};

http_session::http_session(
    boost::asio::ip::tcp::socket socket,
    stream_registry& registry,
    hls_service& hls,
    whep_service& whep)
    : stream_(std::move(socket)), registry_(registry), hls_(hls), whep_(whep), hls_wait_timer_(stream_.get_executor())
{
}

void http_session::start()
{
    stream_.expires_after(std::chrono::seconds(30));
    read_request();
}

void http_session::read_request()
{
    const auto self = shared_from_this();
    http::async_read(stream_, buffer_, request_, [self](boost::system::error_code error, std::size_t bytes) { self->on_request(error, bytes); });
}

void http_session::on_request(boost::system::error_code error, std::size_t bytes)
{
    static_cast<void>(bytes);
    if (error)
    {
        close();
        return;
    }
    handle_request();
}

void http_session::handle_request()
{
    const auto parsed = urls::parse_origin_form(request_.target());
    if (!parsed)
    {
        send_text_response(http::status::bad_request, "text/plain", "bad request target\n");
        return;
    }

    const auto segments = path_segments(*parsed);
    if (segments.empty())
    {
        send_text_response(http::status::not_found, "text/plain", "not found\n");
        return;
    }

    if (segments.front() == "whep")
    {
        handle_whep(*parsed);
        return;
    }

    if (request_.method() != http::verb::get)
    {
        send_text_response(http::status::method_not_allowed, "text/plain", "method not allowed\n");
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

    send_text_response(http::status::not_found, "text/plain", "not found\n");
}

void http_session::handle_whep(const urls::url_view& target)
{
    const auto segments = path_segments(target);
    if (request_.method() == http::verb::post)
    {
        handle_whep_post(segments);
        return;
    }
    if (request_.method() == http::verb::delete_)
    {
        handle_whep_delete(segments);
        return;
    }

    // 本实现只支持一次完整 SDP POST/answer，不支持 PATCH/Trickle ICE。
    send_text_response(http::status::method_not_allowed, "text/plain", "whep supports post and delete only\n");
}

void http_session::handle_whep_post(const std::vector<std::string>& segments)
{
    if (segments.size() < 2 || segments.front() != "whep" || segments[1] == "session")
    {
        send_text_response(http::status::not_found, "text/plain", "not found\n");
        return;
    }

    const auto content_type = request_[http::field::content_type];
    if (!beast::iequals(content_type, "application/sdp"))
    {
        send_text_response(http::status::unsupported_media_type, "text/plain", "content type must be application/sdp\n");
        return;
    }

    const auto stream_name = join_segments(segments, 1, segments.size());
    const auto result = whep_.create(stream_name, request_.body());
    switch (result.error)
    {
    case whep_create_error::none:
        send_whep_response(result.location, result.answer_sdp);
        return;
    case whep_create_error::stream_not_found:
        send_text_response(http::status::not_found, "text/plain", "stream not found\n");
        return;
    case whep_create_error::stream_not_ready:
        send_text_response(http::status::service_unavailable, "text/plain", "stream not ready\n");
        return;
    case whep_create_error::invalid_offer:
        send_text_response(http::status::bad_request, "text/plain", "invalid or unsupported sdp offer\n");
        return;
    case whep_create_error::internal_error:
        send_text_response(http::status::internal_server_error, "text/plain", "whep session create failed\n");
        return;
    }
}

void http_session::handle_whep_delete(const std::vector<std::string>& segments)
{
    if (segments.size() != 3 || segments[0] != "whep" || segments[1] != "session")
    {
        send_text_response(http::status::not_found, "text/plain", "not found\n");
        return;
    }

    if (!whep_.remove(segments[2]))
    {
        send_text_response(http::status::not_found, "text/plain", "whep session not found\n");
        return;
    }
    send_empty_response(http::status::no_content);
}

void http_session::handle_flv(const urls::url_view& target)
{
    auto segments = path_segments(target);
    if (segments.empty() || !segments.back().ends_with(".flv"))
    {
        send_text_response(http::status::not_found, "text/plain", "not found\n");
        return;
    }

    segments.back().resize(segments.back().size() - 4);
    const auto stream_name = join_segments(segments, 0, segments.size());
    auto media_stream = registry_.find(stream_name);
    if (!media_stream)
    {
        send_text_response(http::status::not_found, "text/plain", "stream not found\n");
        return;
    }

    auto response = std::make_shared<http::response<http::empty_body>>(http::status::ok, request_.version());
    response->set(http::field::server, "media_server");
    response->set(http::field::content_type, "video/x-flv");
    response->set(http::field::cache_control, "no-cache");
    response->keep_alive(false);
    response->chunked(true);

    auto serializer = std::make_shared<http::serializer<false, http::empty_body>>(*response);
    stream_.expires_never();
    const auto self = shared_from_this();
    http::async_write_header(stream_, *serializer, [self, response, serializer, media_stream = std::move(media_stream)](boost::system::error_code error, std::size_t bytes) mutable {
        static_cast<void>(response);
        static_cast<void>(serializer);
        static_cast<void>(bytes);
        if (error)
        {
            self->close();
            return;
        }
        self->start_flv(std::move(media_stream));
    });
}

void http_session::handle_hls(const urls::url_view& target)
{
    const auto segments = path_segments(target);
    if (segments.size() < 3 || segments.front() != "hls")
    {
        send_text_response(http::status::not_found, "text/plain", "not found\n");
        return;
    }

    const auto& file = segments.back();
    const auto stream_name = join_segments(segments, 1, segments.size() - 1);
    if (stream_name.empty())
    {
        send_text_response(http::status::not_found, "text/plain", "not found\n");
        return;
    }

    if (file == "index.m3u8")
    {
        const auto count = hls_.segment_count(stream_name);
        if (!count)
        {
            send_text_response(http::status::not_found, "text/plain", "stream not found\n");
            return;
        }
        if (*count == 0)
        {
            wait_hls_playlist(stream_name);
            return;
        }

        const auto playlist = hls_.playlist(stream_name);
        if (!playlist)
        {
            send_text_response(http::status::not_found, "text/plain", "stream not found\n");
            return;
        }
        send_text_response(http::status::ok, "application/vnd.apple.mpegurl", *playlist);
        return;
    }

    if (!file.ends_with(".ts"))
    {
        send_text_response(http::status::not_found, "text/plain", "not found\n");
        return;
    }

    const std::string_view number(file.data(), file.size() - 3);
    std::uint64_t sequence = 0;
    const auto [pointer, parse_error] = std::from_chars(number.data(), number.data() + number.size(), sequence);
    if (parse_error != std::errc{} || pointer != number.data() + number.size())
    {
        send_text_response(http::status::not_found, "text/plain", "not found\n");
        return;
    }

    const auto segment = hls_.segment(stream_name, sequence);
    if (!segment)
    {
        send_text_response(http::status::not_found, "text/plain", "segment not found\n");
        return;
    }
    send_binary_response(http::status::ok, "video/mp2t", *segment);
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

    const auto count = hls_.segment_count(hls_wait_stream_name_);
    if (!count)
    {
        send_text_response(http::status::not_found, "text/plain", "stream not found\n");
        return;
    }

    if (*count > 0)
    {
        const auto playlist = hls_.playlist(hls_wait_stream_name_);
        if (!playlist)
        {
            send_text_response(http::status::not_found, "text/plain", "stream not found\n");
            return;
        }
        send_text_response(http::status::ok, "application/vnd.apple.mpegurl", *playlist);
        return;
    }

    if (std::chrono::steady_clock::now() >= hls_wait_deadline_)
    {
        send_text_response(http::status::service_unavailable, "text/plain", "hls playlist not ready\n");
        return;
    }

    hls_wait_timer_.expires_after(std::chrono::milliseconds(100));
    const auto self = shared_from_this();
    hls_wait_timer_.async_wait([self](boost::system::error_code error) {
        if (!error)
        {
            self->check_hls_playlist();
        }
    });
}

void http_session::send_text_response(http::status status, std::string_view content_type, std::string body)
{
    auto response = std::make_shared<http::response<http::string_body>>(status, request_.version());
    response->set(http::field::server, "media_server");
    response->set(http::field::content_type, content_type);
    response->keep_alive(false);
    response->body() = std::move(body);
    response->prepare_payload();

    const auto self = shared_from_this();
    http::async_write(stream_, *response, [self, response](boost::system::error_code error, std::size_t bytes) {
        static_cast<void>(response);
        static_cast<void>(error);
        static_cast<void>(bytes);
        self->close();
    });
}

void http_session::send_whep_response(std::string location, std::string answer_sdp)
{
    auto response = std::make_shared<http::response<http::string_body>>(http::status::created, request_.version());
    response->set(http::field::server, "media_server");
    response->set(http::field::content_type, "application/sdp");
    response->set(http::field::location, std::move(location));
    response->set(http::field::cache_control, "no-store");
    response->keep_alive(false);
    response->body() = std::move(answer_sdp);
    response->prepare_payload();

    const auto self = shared_from_this();
    http::async_write(stream_, *response, [self, response](boost::system::error_code error, std::size_t bytes) {
        static_cast<void>(response);
        static_cast<void>(error);
        static_cast<void>(bytes);
        self->close();
    });
}

void http_session::send_empty_response(http::status status)
{
    auto response = std::make_shared<http::response<http::empty_body>>(status, request_.version());
    response->set(http::field::server, "media_server");
    response->set(http::field::cache_control, "no-store");
    response->keep_alive(false);

    const auto self = shared_from_this();
    http::async_write(stream_, *response, [self, response](boost::system::error_code error, std::size_t bytes) {
        static_cast<void>(response);
        static_cast<void>(error);
        static_cast<void>(bytes);
        self->close();
    });
}

void http_session::send_binary_response(http::status status, std::string_view content_type, std::vector<std::uint8_t> body)
{
    auto response = std::make_shared<http::response<http::vector_body<std::uint8_t>>>(status, request_.version());
    response->set(http::field::server, "media_server");
    response->set(http::field::content_type, content_type);
    response->keep_alive(false);
    response->body() = std::move(body);
    response->prepare_payload();

    const auto self = shared_from_this();
    http::async_write(stream_, *response, [self, response](boost::system::error_code error, std::size_t bytes) {
        static_cast<void>(response);
        static_cast<void>(error);
        static_cast<void>(bytes);
        self->close();
    });
}

void http_session::start_flv(std::shared_ptr<media_stream> media_stream)
{
    keep_alive_ = shared_from_this();
    media_stream_ = std::move(media_stream);
    const auto tracks = media_stream_->tracks();
    const std::weak_ptr<http_session> weak_self = shared_from_this();
    flv_output_ = std::make_shared<http_flv_output>(
        tracks,
        [weak_self](std::span<const std::uint8_t> data) {
            if (const auto self = weak_self.lock())
            {
                self->enqueue_flv(data);
            }
        },
        [weak_self]() {
            if (const auto self = weak_self.lock())
            {
                self->finish_flv();
            }
        });

    if (!media_stream_->add_sink(flv_output_))
    {
        finish_flv();
    }
}

void http_session::enqueue_flv(std::span<const std::uint8_t> data)
{
    if (closed_ || flv_finishing_ || data.empty())
    {
        return;
    }

    flv_chunks_.push_back(std::make_shared<flv_chunk>(data));
    if (!flv_writing_)
    {
        write_flv_chunk();
    }
}

void http_session::write_flv_chunk()
{
    if (closed_)
    {
        return;
    }
    if (flv_chunks_.empty())
    {
        flv_writing_ = false;
        if (flv_finishing_)
        {
            finish_flv();
        }
        return;
    }

    flv_writing_ = true;
    const auto chunk = flv_chunks_.front();
    const auto self = shared_from_this();
    net::async_write(stream_, chunk->chunk, [self, chunk](boost::system::error_code error, std::size_t bytes) {
        static_cast<void>(chunk);
        static_cast<void>(bytes);
        if (error)
        {
            self->close();
            return;
        }
        self->flv_chunks_.pop_front();
        self->write_flv_chunk();
    });
}

void http_session::finish_flv()
{
    if (closed_)
    {
        return;
    }
    if (flv_writing_ || !flv_chunks_.empty())
    {
        flv_finishing_ = true;
        return;
    }

    flv_finishing_ = true;
    auto last = std::make_shared<decltype(http::make_chunk_last())>(http::make_chunk_last());
    const auto self = shared_from_this();
    net::async_write(stream_, *last, [self, last](boost::system::error_code error, std::size_t bytes) {
        static_cast<void>(last);
        static_cast<void>(error);
        static_cast<void>(bytes);
        self->close();
    });
}

void http_session::detach_flv()
{
    if (media_stream_ && flv_output_)
    {
        static_cast<void>(media_stream_->remove_sink(flv_output_.get()));
    }
    flv_output_.reset();
    media_stream_.reset();
    flv_chunks_.clear();
}

void http_session::close()
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
    keep_alive_.reset();
}

std::vector<std::string> http_session::path_segments(const urls::url_view& target)
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
