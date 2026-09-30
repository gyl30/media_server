#include <vector>
#include <utility>

#include <spdlog/spdlog.h>
#include <boost/asio/post.hpp>
#include <boost/url/parse.hpp>
#include <boost/asio/detached.hpp>

#include "media/rtmp/rtmp_session.h"
#include "media/net/worker_context.h"
#include "media/core/stream_registry.h"
#include "media/rtmp/rtmp_play_session.h"
#include "media/rtmp/rtmp_publish_session.h"

extern "C"
{
#include "flv-proto.h"
#include "rtmp-server.h"
}

namespace media_server
{

std::optional<std::string> parse_rtmp_target(std::string_view app, std::string_view stream)
{
    const auto parsed = boost::urls::parse_relative_ref(stream);
    if (!parsed || parsed->has_query() || parsed->has_fragment())
    {
        return std::nullopt;
    }

    std::string path;
    for (const auto segment : parsed->segments())
    {
        if (!path.empty())
        {
            path.push_back('/');
        }
        path.append(segment);
    }
    if (path.empty())
    {
        return std::nullopt;
    }

    std::string stream_name;
    if (!app.empty())
    {
        stream_name.append(app);
        stream_name.push_back('/');
    }
    stream_name.append(path);
    return stream_name;
}

rtmp_session::rtmp_session(worker_context& worker, boost::asio::ip::tcp::socket socket, std::size_t max_write_queue_bytes)
    : worker_(worker), transport_(std::make_shared<tcp_transport>(std::move(socket), max_write_queue_bytes))
{
}

void rtmp_session::startup()
{
    const auto self = shared_from_this();
    transport_->set_write_callback(
        [weak = std::weak_ptr<rtmp_session>(self)](boost::system::error_code error, std::size_t)
        {
            if (error)
            {
                if (const auto owner = weak.lock())
                {
                    owner->shutdown();
                }
            }
        });
    worker_.spawn([self](boost::asio::yield_context yield) { self->run(yield); });
}

void rtmp_session::run(boost::asio::yield_context yield)
{
    rtmp_server_handler_t handler{};
    handler.send = &rtmp_session::send_callback;
    handler.ondelete_stream = &rtmp_session::delete_stream_callback;
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
        shutdown();
        return;
    }
    rtmp_context_ = context;
    run_read(context, yield);

    rtmp_context_ = nullptr;
    rtmp_server_destroy(context);
    safe_shutdown();
    spdlog::debug("rtmp shutdown");
}

void rtmp_session::run_read(rtmp_server_t* context, boost::asio::yield_context yield)
{
    std::vector<std::uint8_t> buffer(64 * 1024);
    for (;;)
    {
        boost::system::error_code error;
        const auto bytes = transport_->read(buffer, yield, error);
        if (error)
        {
            break;
        }
        const auto input_result = bytes == 0 ? 0 : rtmp_server_input(context, buffer.data(), bytes);
        if (input_result != 0)
        {
            if (input_result != RTMP_SERVER_INPUT_STOP)
            {
                shutdown();
            }
            break;
        }
    }
}

int rtmp_session::send_callback(void* param, const void* header, std::size_t header_bytes, const void* payload, std::size_t payload_bytes)
{
    auto* self = static_cast<rtmp_session*>(param);
    std::vector<std::uint8_t> data;
    data.reserve(header_bytes + payload_bytes);
    if (header_bytes != 0)
    {
        const auto* first = static_cast<const std::uint8_t*>(header);
        data.insert(data.end(), first, first + header_bytes);
    }
    if (payload_bytes != 0)
    {
        const auto* first = static_cast<const std::uint8_t*>(payload);
        data.insert(data.end(), first, first + payload_bytes);
    }
    self->transport_->write(std::move(data));
    return static_cast<int>(header_bytes + payload_bytes);
}

int rtmp_session::delete_stream_callback(void* param, std::uint32_t stream_id)
{
    return static_cast<rtmp_session*>(param)->on_delete_stream(stream_id);
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
    return self->publish_ ? self->publish_->on_video(data, bytes, timestamp) : -1;
}

int rtmp_session::audio_callback(void* param, const void* data, std::size_t bytes, std::uint32_t timestamp)
{
    auto* self = static_cast<rtmp_session*>(param);
    return self->publish_ ? self->publish_->on_audio(data, bytes, timestamp) : -1;
}

int rtmp_session::script_callback(void* param, const void* data, std::size_t bytes, std::uint32_t)
{
    auto* self = static_cast<rtmp_session*>(param);
    if (!self->publish_)
    {
        return 0;
    }
    return self->publish_->on_script(std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(data), bytes));
}

int rtmp_session::duration_callback(void*, const char*, const char*, double* duration)
{
    if (duration != nullptr)
    {
        *duration = 0.0;
    }
    return 0;
}

int rtmp_session::on_delete_stream(std::uint32_t stream_id)
{
    if (stream_id == 0)
    {
        return 0;
    }

    shutdown();
    return RTMP_SERVER_INPUT_STOP;
}

int rtmp_session::on_play(std::string app, std::string stream)
{
    if (publish_ || play_)
    {
        return -1;
    }

    const auto target = parse_rtmp_target(app, stream);
    if (!target)
    {
        shutdown();
        return -1;
    }

    auto media = stream_registry::instance().find(*target);
    if (!media)
    {
        return -1;
    }

    const auto self = shared_from_this();
    play_ = std::make_shared<rtmp_play_session>(
        worker_,
        std::move(media),
        [self](int type, std::span<const std::uint8_t> data, std::uint32_t timestamp)
        {
            if (self->rtmp_context_ == nullptr)
            {
                return -1;
            }
            if (type == FLV_TYPE_VIDEO)
            {
                return rtmp_server_send_video(self->rtmp_context_, data.data(), data.size(), timestamp);
            }
            if (type == FLV_TYPE_AUDIO)
            {
                return rtmp_server_send_audio(self->rtmp_context_, data.data(), data.size(), timestamp);
            }
            return 0;
        },
        [self]() { self->shutdown(); });
    boost::asio::post(worker_.io(), [self]()
                      {
                          if (self->play_)
                          {
                              self->play_->startup();
                          }
                      });
    spdlog::info("rtmp play {}", *target);
    return 0;
}

int rtmp_session::on_publish(std::string app, std::string stream)
{
    if (publish_ || play_)
    {
        return -1;
    }

    const auto target = parse_rtmp_target(app, stream);
    if (!target)
    {
        shutdown();
        return -1;
    }

    const auto self = shared_from_this();
    auto publish = std::make_shared<rtmp_publish_session>(worker_, *target, [self]() { self->shutdown(); });
    if (!publish->startup())
    {
        publish->shutdown();
        return -1;
    }

    publish_ = std::move(publish);
    spdlog::info("rtmp publish {}", *target);
    return 0;
}

void rtmp_session::shutdown()
{
    const auto self = shared_from_this();
    boost::asio::post(worker_.io(), [self]() { self->safe_shutdown(); });
}

void rtmp_session::safe_shutdown()
{
    rtmp_context_ = nullptr;
    if (publish_)
    {
        publish_->shutdown();
        publish_.reset();
    }
    if (play_)
    {
        play_->shutdown();
        play_.reset();
    }
    transport_->shutdown();
}

}    // namespace media_server
