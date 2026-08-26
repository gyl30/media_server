#include <utility>

#include <spdlog/spdlog.h>
#include <boost/asio/post.hpp>

#include "media/rtmp/rtmp_session.h"
#include "media/core/stream_registry.h"
#include "media/rtmp/rtmp_input_session.h"
#include "media/rtmp/rtmp_output_session.h"

extern "C"
{
#include "flv-proto.h"
#include "rtmp-server.h"
}

namespace media_server
{

rtmp_session::rtmp_session(std::shared_ptr<tcp_connection> connection, output_video_config video, std::chrono::milliseconds initial_tracks_timeout)
    : connection_(std::move(connection)), initial_tracks_timeout_(initial_tracks_timeout), video_config_(video)
{
}

rtmp_session::~rtmp_session() = default;

void rtmp_session::startup()
{
    rtmp_server_handler_t handler{};
    handler.send = &rtmp_session::send_callback;
    handler.onplay = &rtmp_session::play_callback;
    handler.onpause = &rtmp_session::pause_callback;
    handler.onseek = &rtmp_session::seek_callback;
    handler.onpublish = &rtmp_session::publish_callback;
    handler.onvideo = &rtmp_session::video_callback;
    handler.onaudio = &rtmp_session::audio_callback;
    handler.onscript = &rtmp_session::script_callback;
    handler.ongetduration = &rtmp_session::duration_callback;

    server_ = rtmp_server_create(this, &handler);
    if (server_ == nullptr)
    {
        shutdown();
        return;
    }

    const auto self = shared_from_this();
    connection_->startup([self](boost::system::error_code error, std::span<const std::uint8_t> data) { self->on_tcp_read(error, data); },
                         [self](boost::system::error_code error, std::size_t write_size) { self->on_tcp_write(error, write_size); });
}

int rtmp_session::send_callback(void* param, const void* header, std::size_t header_bytes, const void* payload, std::size_t payload_bytes)
{
    auto* self = static_cast<rtmp_session*>(param);
    self->connection_->write(header, header_bytes);
    self->connection_->write(payload, payload_bytes);
    return static_cast<int>(header_bytes + payload_bytes);
}

int rtmp_session::play_callback(void* param, const char* app, const char* stream, double, double, std::uint8_t)
{
    return static_cast<rtmp_session*>(param)->on_play(app != nullptr ? app : "", stream != nullptr ? stream : "");
}

int rtmp_session::pause_callback(void*, int, std::uint32_t) { return -1; }

int rtmp_session::seek_callback(void*, std::uint32_t) { return -1; }

int rtmp_session::publish_callback(void* param, const char* app, const char* stream, const char*)
{
    return static_cast<rtmp_session*>(param)->on_publish(app != nullptr ? app : "", stream != nullptr ? stream : "");
}

int rtmp_session::video_callback(void* param, const void* data, std::size_t bytes, std::uint32_t timestamp)
{
    auto* self = static_cast<rtmp_session*>(param);
    return self->input_ ? self->input_->on_video(data, bytes, timestamp) : -1;
}

int rtmp_session::audio_callback(void* param, const void* data, std::size_t bytes, std::uint32_t timestamp)
{
    auto* self = static_cast<rtmp_session*>(param);
    return self->input_ ? self->input_->on_audio(data, bytes, timestamp) : -1;
}

int rtmp_session::script_callback(void* param, const void* data, std::size_t bytes, std::uint32_t)
{
    auto* self = static_cast<rtmp_session*>(param);
    if (!self->input_)
    {
        return 0;
    }
    return self->input_->on_script(std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(data), bytes));
}

int rtmp_session::duration_callback(void*, const char*, const char*, double* duration)
{
    if (duration != nullptr)
    {
        *duration = 0.0;
    }
    return 0;
}

int rtmp_session::on_play(std::string app, std::string stream)
{
    if (input_ || output_)
    {
        return -1;
    }

    stream_name_ = make_stream_name(app, stream);
    auto media = registry::instance().find(stream_name_);
    if (!media)
    {
        spdlog::warn("rtmp play stream not found {}", stream_name_);
        return -1;
    }
    if (video_config_.codec == output_video_codec::av1 && !rtmp_server_peer_supports_fourcc(server_, "av01"))
    {
        spdlog::warn("rtmp play av1 unsupported by peer {}", stream_name_);
        return -1;
    }

    const std::weak_ptr<rtmp_session> weak = shared_from_this();
    output_ = std::make_shared<rtmp_output_session>(
        connection_->socket().get_executor(),
        std::move(media),
        [weak](int type, std::span<const std::uint8_t> data, std::uint32_t timestamp)
        {
            const auto self = weak.lock();
            if (!self || self->closed_ || self->server_ == nullptr)
            {
                return;
            }
            if (type == FLV_TYPE_VIDEO)
            {
                static_cast<void>(rtmp_server_send_video(self->server_, data.data(), data.size(), timestamp));
            }
            else if (type == FLV_TYPE_AUDIO)
            {
                static_cast<void>(rtmp_server_send_audio(self->server_, data.data(), data.size(), timestamp));
            }
        },
        video_config_,
        [weak]()
        {
            if (const auto self = weak.lock())
            {
                self->shutdown();
            }
        });

    const auto self = shared_from_this();
    boost::asio::post(connection_->socket().get_executor(),
                      [self]()
                      {
                          if (self->closed_ || self->server_ == nullptr || !self->output_)
                          {
                              return;
                          }
                          if (rtmp_server_start(self->server_, 0, nullptr) != 0)
                          {
                              self->shutdown();
                              return;
                          }
                          self->output_->startup();
                          spdlog::info("rtmp play {}", self->stream_name_);
                      });

    return RTMP_SERVER_ASYNC_START;
}

int rtmp_session::on_publish(std::string app, std::string stream)
{
    if (input_ || output_)
    {
        return -1;
    }

    stream_name_ = make_stream_name(app, stream);
    const std::weak_ptr<rtmp_session> weak = shared_from_this();
    auto input = std::make_shared<rtmp_input_session>(connection_->socket().get_executor(),
                                                      stream_name_,
                                                      initial_tracks_timeout_,
                                                      [weak]()
                                                      {
                                                          if (const auto self = weak.lock())
                                                          {
                                                              self->shutdown();
                                                          }
                                                      });
    if (!input->startup())
    {
        return -1;
    }
    input_ = std::move(input);
    spdlog::info("rtmp publish {}", stream_name_);
    return 0;
}

void rtmp_session::on_tcp_read(boost::system::error_code error, std::span<const std::uint8_t> data)
{
    if (error)
    {
        shutdown();
        return;
    }
    if (server_ == nullptr || closed_)
    {
        return;
    }
    if (rtmp_server_input(server_, data.data(), data.size()) != 0)
    {
        shutdown();
    }
}

void rtmp_session::on_tcp_write(boost::system::error_code error, std::size_t write_size)
{
    static_cast<void>(write_size);
    if (error)
    {
        shutdown();
    }
}

void rtmp_session::shutdown()
{
    const auto self = shared_from_this();
    boost::asio::post(connection_->socket().get_executor(), [self]() { self->safe_shutdown(); });
}

void rtmp_session::safe_shutdown()
{
    if (closed_)
    {
        return;
    }
    closed_ = true;
    if (input_)
    {
        input_->shutdown();
        input_.reset();
    }
    if (output_)
    {
        output_->shutdown();
        output_.reset();
    }
    connection_->shutdown();
    if (server_ != nullptr)
    {
        rtmp_server_destroy(server_);
        server_ = nullptr;
    }
    spdlog::debug("rtmp shutdown {}", stream_name_);
}

std::string rtmp_session::make_stream_name(std::string_view app, std::string_view stream)
{
    if (app.empty())
    {
        return std::string(stream);
    }
    if (stream.empty())
    {
        return std::string(app);
    }
    return std::string(app) + '/' + std::string(stream);
}

}    // namespace media_server
