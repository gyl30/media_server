#include <array>
#include <utility>
#include <algorithm>

#include <boost/asio/post.hpp>
#include <boost/asio/write.hpp>
#include <boost/asio/detached.hpp>
#include <boost/beast/http/chunk_encode.hpp>

#include "media/http/http_target.h"
#include "media/net/worker_context.h"
#include "media/core/stream_registry.h"
#include "media/http/http_flv_session.h"
#include "media/http/signaling_verify.h"

extern "C"
{
#include "flv-proto.h"
}

namespace media_server
{

http_flv_session::http_flv_session(worker_context& worker, boost::beast::tcp_stream stream, request_type request, const config& application_config)
    : worker_(worker), stream_(std::move(stream)), request_(std::move(request)), config_(application_config)
{
}

void http_flv_session::startup()
{
    const auto self = shared_from_this();
    worker_.spawn([self](boost::asio::yield_context yield) { self->run(yield); });
}

void http_flv_session::run(boost::asio::yield_context yield)
{
    handle_request(yield);
    shutdown();
}

void http_flv_session::handle_request(boost::asio::yield_context& yield)
{
    if (request_.method() != boost::beast::http::verb::get)
    {
        send_text_response(boost::beast::http::status::method_not_allowed, "text/plain", "method not allowed\n", yield, "GET");
        return;
    }

    const std::string_view raw_target(request_.target().data(), request_.target().size());
    if (raw_target.find_first_of("%?#") != std::string_view::npos)
    {
        send_text_response(boost::beast::http::status::bad_request, "text/plain", "invalid playback target\n", yield);
        return;
    }
    const auto target = parse_http_target(raw_target);
    if (!target)
    {
        send_text_response(boost::beast::http::status::bad_request, "text/plain", "invalid playback target\n", yield);
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
    if (path.size() != 2 || !path[1].ends_with(".flv"))
    {
        send_text_response(boost::beast::http::status::not_found, "text/plain", "not found\n", yield);
        return;
    }

    path[1].resize(path[1].size() - 4);
    const auto& stream_id = path[0];
    const auto& token = path[1];
    if (!valid_stream_id(stream_id) || !valid_stream_token(token))
    {
        send_text_response(boost::beast::http::status::bad_request, "text/plain", "invalid playback target\n", yield);
        return;
    }

    auto media_stream = stream_registry::instance().find(stream_id);
    if (!media_stream)
    {
        send_text_response(boost::beast::http::status::not_found, "text/plain", "stream not found\n", yield);
        return;
    }

    if (!verify_stream(config_, token, "play", stream_id, yield))
    {
        send_text_response(boost::beast::http::status::forbidden, "text/plain", "playback denied\n", yield);
        return;
    }

    bool has_audio = false;
    bool has_video = false;
    for (const auto& track : media_stream->tracks())
    {
        has_audio = has_audio || track.kind == media_kind::audio;
        has_video = has_video || track.kind == media_kind::video;
    }
    writer_ = flv_writer_create2(has_audio ? 1 : 0, has_video ? 1 : 0, &http_flv_session::writer_callback, this);
    if (writer_ == nullptr)
    {
        send_text_response(boost::beast::http::status::internal_server_error, "text/plain", "flv writer unavailable\n", yield);
        return;
    }
    auto output = media_stream->flv_output();
    if (!output)
    {
        send_text_response(boost::beast::http::status::bad_request, "text/plain", "unsupported flv track\n", yield);
        return;
    }
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
            return;
        }
    }
    if (!enqueue(std::move(output_buffer_)))
    {
        return;
    }
    output_ = std::move(output);
    output_->add_sink(shared_from_this());

    std::array<std::uint8_t, 1> read_buffer{};
    for (;;)
    {
        boost::system::error_code error;
        stream_.async_read_some(boost::asio::buffer(read_buffer), yield[error]);
        if (error)
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

bool http_flv_session::enqueue(std::vector<std::uint8_t> data)
{
    if (data.empty())
    {
        return true;
    }
    if (queued_output_bytes_ > max_queued_output_bytes_ || data.size() > max_queued_output_bytes_ - queued_output_bytes_)
    {
        return false;
    }

    const bool start_writer = output_queue_.empty();
    queued_output_bytes_ += data.size();
    output_queue_.push_back(std::move(data));
    if (start_writer)
    {
        start_write();
    }
    return true;
}

void http_flv_session::start_write()
{
    const auto self = shared_from_this();
    std::vector<boost::asio::const_buffer> buffers;
    buffers.reserve(output_queue_.size());
    for (const auto& data : output_queue_)
    {
        buffers.push_back(boost::asio::buffer(data));
    }
    const auto chunk = boost::beast::http::make_chunk(buffers);
    boost::asio::async_write(stream_,
                             chunk,
                             [self, count = buffers.size()](boost::system::error_code error, std::size_t)
                             {
                                 if (error)
                                 {
                                     self->output_queue_.clear();
                                     self->queued_output_bytes_ = 0;
                                     self->shutdown();
                                     return;
                                 }

                                 for (std::size_t index = 0; index < count; ++index)
                                 {
                                     self->queued_output_bytes_ -= self->output_queue_.front().size();
                                     self->output_queue_.pop_front();
                                 }
                                 if (!self->output_queue_.empty())
                                 {
                                     self->start_write();
                                 }
                             });
}

void http_flv_session::on_end()
{
    if (output_)
    {
        shutdown();
    }
}

void http_flv_session::on_flv_tag(const flv_tag& tag)
{
    if (!output_)
    {
        return;
    }
    output_buffer_.clear();
    if (!started_)
    {
        if (!tag.key_frame)
        {
            return;
        }
        started_ = true;
        const auto& headers = *tag.headers;
        if ((headers.video && flv_writer_input(writer_, FLV_TYPE_VIDEO, headers.video->data(), headers.video->size(), tag.timestamp) != 0) ||
            (headers.audio && flv_writer_input(writer_, FLV_TYPE_AUDIO, headers.audio->data(), headers.audio->size(), tag.timestamp) != 0))
        {
            shutdown();
            return;
        }
    }
    if (flv_writer_input(writer_, tag.type, tag.data->data(), tag.data->size(), tag.timestamp) != 0 || !enqueue(std::move(output_buffer_)))
    {
        shutdown();
    }
}

int http_flv_session::writer_callback(void* param, const flv_vec_t* vectors, int count)
{
    auto* self = static_cast<http_flv_session*>(param);
    if (vectors == nullptr || count <= 0)
    {
        return -1;
    }

    std::size_t bytes = 0;
    for (int index = 0; index < count; ++index)
    {
        if (vectors[index].ptr != nullptr && vectors[index].len > 0)
        {
            bytes += static_cast<std::size_t>(vectors[index].len);
        }
    }

    self->output_buffer_.reserve(self->output_buffer_.size() + bytes);
    for (int index = 0; index < count; ++index)
    {
        if (vectors[index].ptr == nullptr || vectors[index].len <= 0)
        {
            continue;
        }
        const auto* begin = static_cast<const std::uint8_t*>(vectors[index].ptr);
        self->output_buffer_.insert(self->output_buffer_.end(), begin, begin + vectors[index].len);
    }
    return 0;
}

void http_flv_session::shutdown()
{
    const auto self = shared_from_this();
    boost::asio::post(worker_.io(), [self]() { self->safe_shutdown(); });
}

void http_flv_session::safe_shutdown()
{
    output_.reset();
    if (writer_ != nullptr)
    {
        flv_writer_destroy(writer_);
        writer_ = nullptr;
    }
    output_buffer_.clear();
    boost::system::error_code error;
    stream_.socket().shutdown(boost::asio::ip::tcp::socket::shutdown_both, error);
    stream_.socket().close(error);
}

}    // namespace media_server
