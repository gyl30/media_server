#include <array>
#include <utility>

#include <spdlog/spdlog.h>
#include <boost/asio/post.hpp>
#include <boost/asio/write.hpp>
#include <boost/asio/detached.hpp>

#include "media/rtmp/rtmp_session.h"
#include "media/net/worker_context.h"
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
namespace
{
constexpr auto slow_write_timeout = std::chrono::seconds(15);
}

rtmp_session::rtmp_session(worker_context& worker,
                           boost::asio::ip::tcp::socket socket,
                           output_video_config video,
                           std::chrono::milliseconds initial_tracks_timeout)
    : worker_(worker), socket_(std::move(socket)), initial_tracks_timeout_(initial_tracks_timeout), video_config_(video)
{
}

rtmp_session::~rtmp_session() = default;

void rtmp_session::startup()
{
    const auto self = shared_from_this();
    boost::asio::spawn(worker_.io(), [self](boost::asio::yield_context yield) { self->run(yield); }, boost::asio::detached);
}

void rtmp_session::run(boost::asio::yield_context yield)
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

    auto* context = rtmp_server_create(this, &handler);
    if (context == nullptr)
    {
        safe_shutdown();
        return;
    }
    rtmp_context_ = context;

    std::array<std::uint8_t, 64 * 1024> buffer{};
    for (;;)
    {
        boost::system::error_code error;
        const auto bytes = socket_.async_read_some(boost::asio::buffer(buffer), yield[error]);
        if (error)
        {
            break;
        }
        if (bytes != 0 && rtmp_server_input(context, buffer.data(), bytes) != 0)
        {
            break;
        }
    }

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
    rtmp_context_ = nullptr;
    rtmp_server_destroy(context);
    safe_shutdown();
    spdlog::debug("rtmp shutdown {}", stream_name_);
}

int rtmp_session::send_callback(void* param, const void* header, std::size_t header_bytes, const void* payload, std::size_t payload_bytes)
{
    auto* self = static_cast<rtmp_session*>(param);
    auto data = std::make_shared<std::vector<std::uint8_t>>();
    data->reserve(header_bytes + payload_bytes);
    if (header_bytes != 0)
    {
        const auto* first = static_cast<const std::uint8_t*>(header);
        data->insert(data->end(), first, first + header_bytes);
    }
    if (payload_bytes != 0)
    {
        const auto* first = static_cast<const std::uint8_t*>(payload);
        data->insert(data->end(), first, first + payload_bytes);
    }
    self->write(std::move(data));
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

void rtmp_session::write(std::shared_ptr<std::vector<std::uint8_t>> data)
{
    if (data->empty())
    {
        return;
    }

    const bool start_write = write_queue_.empty();
    write_queue_.push_back(std::move(data));
    if (start_write)
    {
        const auto self = shared_from_this();
        boost::asio::spawn(worker_.io(), [self](boost::asio::yield_context yield) { self->run_write(yield); }, boost::asio::detached);
    }
}

void rtmp_session::run_write(boost::asio::yield_context yield)
{
    for (;;)
    {
        if (write_queue_.empty())
        {
            return;
        }

        const auto data = write_queue_.front();
        boost::system::error_code error;
        const auto started_at = std::chrono::steady_clock::now();
        static_cast<void>(boost::asio::async_write(socket_, boost::asio::buffer(*data), yield[error]));
        if (error)
        {
            write_queue_.clear();
            shutdown();
            return;
        }

        write_queue_.pop_front();
        if (std::chrono::steady_clock::now() - started_at > slow_write_timeout)
        {
            write_queue_.clear();
            shutdown();
            return;
        }
    }
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
    if (video_config_.codec == output_video_codec::av1 && !rtmp_server_peer_supports_fourcc(rtmp_context_, "av01"))
    {
        spdlog::warn("rtmp play av1 unsupported by peer {}", stream_name_);
        return -1;
    }

    const std::weak_ptr<rtmp_session> weak = shared_from_this();
    output_ = std::make_shared<rtmp_output_session>(
        worker_,
        std::move(media),
        [weak](int type, std::span<const std::uint8_t> data, std::uint32_t timestamp)
        {
            const auto self = weak.lock();
            if (!self || self->rtmp_context_ == nullptr)
            {
                return;
            }
            if (type == FLV_TYPE_VIDEO)
            {
                static_cast<void>(rtmp_server_send_video(self->rtmp_context_, data.data(), data.size(), timestamp));
            }
            else if (type == FLV_TYPE_AUDIO)
            {
                static_cast<void>(rtmp_server_send_audio(self->rtmp_context_, data.data(), data.size(), timestamp));
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
    boost::asio::post(worker_.io(),
                      [self]()
                      {
                          if (self->rtmp_context_ == nullptr || !self->output_)
                          {
                              return;
                          }
                          if (rtmp_server_start(self->rtmp_context_, 0, nullptr) != 0)
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
    auto input = std::make_shared<rtmp_input_session>(worker_,
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

void rtmp_session::shutdown()
{
    const auto self = shared_from_this();
    boost::asio::post(worker_.io(), [self]() { self->safe_shutdown(); });
}

void rtmp_session::safe_shutdown()
{
    if (closed_)
    {
        return;
    }
    closed_ = true;
    rtmp_context_ = nullptr;
    write_queue_.clear();

    boost::system::error_code error;
    socket_.cancel(error);
    socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, error);
    socket_.close(error);
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
