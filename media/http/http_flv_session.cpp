#include <array>
#include <span>
#include <utility>

#include <boost/url/parse.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/write.hpp>
#include <boost/asio/detached.hpp>
#include <boost/beast/http/chunk_encode.hpp>

#include "media/net/worker_context.h"
#include "media/codec/av1_video_egress.h"
#include "media/core/stream_registry.h"
#include "media/http/http_flv_session.h"

namespace media_server
{

http_flv_session::http_flv_session(worker_context& worker, boost::beast::tcp_stream stream, request_type request, const config& config)
    : worker_(worker),
      stream_(std::move(stream)),
      request_(std::move(request)),
      muxer_(
          [this](int type, std::span<const std::uint8_t> data, std::uint32_t timestamp)
          {
              if (writer_ != nullptr)
              {
                  flv_writer_input(writer_, type, data.data(), data.size(), timestamp);
              }
          },
          config.http_video),
      av1_output_(config.http_video.codec == video_transcode_codec::av1)
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

    if (target.has_query() || stream_name_.empty())
    {
        send_text_response(boost::beast::http::status::bad_request, "text/plain", "invalid stream name\n", yield);
        return;
    }

    auto media_stream = stream_registry::instance().find(stream_name_);
    if (!media_stream)
    {
        send_text_response(boost::beast::http::status::not_found, "text/plain", "stream not found\n", yield);
        return;
    }

    if (av1_output_)
    {
        for (const auto& track : media_stream->tracks())
        {
            if (track.kind == media_kind::video && (track.codec == codec_id::h264 || track.codec == codec_id::h265))
            {
                video_egress_ = acquire_av1_video_egress(media_stream, worker_, std::nullopt);
                if (!video_egress_)
                {
                    send_text_response(boost::beast::http::status::unsupported_media_type, "text/plain", "av1 output unavailable\n", yield);
                    return;
                }
                media_stream = video_egress_->stream();
                break;
            }
        }
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
        if (closed_)
        {
            return;
        }
    }

    media_stream->add_reader(shared_from_this(), worker_);

    std::array<std::uint8_t, 1> read_buffer{};
    for (;;)
    {
        boost::system::error_code error;
        stream_.async_read_some(boost::asio::buffer(read_buffer), yield[error]);
        if (error)
        {
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
        write_complete(generation);
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
            write_complete(generation);
            return;
        }

        generation = pending_generation_;
        data = std::move(pending_bootstrap_);
        pending_bootstrap_ready_ = false;
        if (data.empty())
        {
            write_in_progress_ = false;
            write_complete(generation);
            return;
        }
    }
}

void http_flv_session::on_tracks(media_tracks_ptr tracks)
{
    if (!closed_)
    {
        apply_tracks(tracks);
    }
}

void http_flv_session::on_media_available()
{
    if (closed_ || write_in_progress_)
    {
        return;
    }
    process_read();
}

void http_flv_session::on_end()
{
    if (closed_)
    {
        return;
    }
    shutdown();
}

void http_flv_session::write_complete(std::uint64_t generation)
{
    if (!closed_ && generation_ == generation)
    {
        process_read();
    }
}

bool http_flv_session::apply_tracks(const media_tracks_ptr& tracks)
{
    if (!tracks || writer_ != nullptr)
    {
        return false;
    }

    bool has_audio = false;
    bool has_video = false;
    for (const auto& track : *tracks)
    {
        has_audio = has_audio || track.kind == media_kind::audio;
        has_video = has_video || track.kind == media_kind::video;
    }

    output_buffer_.clear();
    if (writer_ == nullptr)
    {
        writer_ = flv_writer_create2(has_audio ? 1 : 0, has_video ? 1 : 0, &http_flv_session::writer_callback, this);
    }
    for (const auto& track : *tracks)
    {
        reader_tracks_.emplace(track.id, track);
        muxer_.on_track(track);
    }

    ++generation_;
    enqueue(generation_, std::move(output_buffer_), true);
    return true;
}

void http_flv_session::process_read()
{
    if (closed_)
    {
        return;
    }
    for (;;)
    {
        auto entry = read();
        if (!entry)
        {
            return;
        }
        const auto track = reader_tracks_.find(entry->frame.track);
        if (track == reader_tracks_.end())
        {
            continue;
        }
        if (waiting_for_key_frame_)
        {
            if (track->second.kind != media_kind::video || !entry->frame.key_frame)
            {
                continue;
            }
            waiting_for_key_frame_ = false;
        }

        output_buffer_.clear();
        muxer_.on_frame(entry->frame);
        if (output_buffer_.empty())
        {
            continue;
        }
        enqueue(generation_, std::move(output_buffer_), false);
        return;
    }
}

int http_flv_session::writer_callback(void* param, const flv_vec_t* vectors, int count)
{
    auto* self = static_cast<http_flv_session*>(param);
    if (self->closed_ || vectors == nullptr || count <= 0)
    {
        return 0;
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
    if (closed_)
    {
        return;
    }
    closed_ = true;
    remove_reader();
    muxer_.shutdown();
    release_av1_video_egress(video_egress_);
    if (writer_ != nullptr)
    {
        flv_writer_destroy(writer_);
        writer_ = nullptr;
    }
    reader_tracks_.clear();
    output_buffer_.clear();
    pending_bootstrap_.clear();
    pending_bootstrap_ready_ = false;
    write_in_progress_ = false;
    boost::system::error_code error;
    stream_.socket().shutdown(boost::asio::ip::tcp::socket::shutdown_both, error);
    stream_.socket().close(error);
}

}    // namespace media_server
