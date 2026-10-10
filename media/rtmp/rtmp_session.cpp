#include <vector>
#include <utility>

#include <spdlog/spdlog.h>
#include <boost/asio/post.hpp>
#include <boost/url/parse.hpp>

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

    std::string stream_id;
    if (!app.empty())
    {
        stream_id.append(app);
        stream_id.push_back('/');
    }
    stream_id.append(path);
    return stream_id;
}

rtmp_session::rtmp_session(worker_context& worker, boost::asio::ip::tcp::socket socket)
    : worker_(worker), transport_(std::make_shared<tcp_transport>(std::move(socket))), idle_timer_(worker.io())
{
}

rtmp_session::~rtmp_session() = default;

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
    // 收到数据即刷新；播放连接停止计时。
    idle_timer_.start(self,
                      [this]()
                      {
                          spdlog::info("rtmp input idle timeout");
                          shutdown();
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
        safe_shutdown();
        return;
    }
    rtmp_context_ = context;
    run_read(context, yield);

    rtmp_server_destroy(context);
    rtmp_context_ = nullptr;
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
        idle_timer_.touch();
        if (bytes != 0 && rtmp_server_input(context, buffer.data(), bytes) != 0)
        {
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

int rtmp_session::delete_stream_callback(void*, std::uint32_t) { return RTMP_SERVER_INPUT_STOP; }

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
    *duration = 0.0;
    return 0;
}

int rtmp_session::on_play(std::string_view app, std::string_view stream)
{
    if (publish_ || play_)
    {
        return -1;
    }

    const auto target = parse_rtmp_target(app, stream);
    if (!target)
    {
        return -1;
    }

    auto media = stream_registry::instance().find(*target);
    if (!media)
    {
        return -1;
    }

    const auto self = shared_from_this();
    // 回调返回 -1 会结束读循环，失败时 play_ 由 safe_shutdown 清理。
    play_ = std::make_shared<rtmp_play_session>(
        worker_,
        std::move(media),
        [self](int type, std::span<const std::uint8_t> data, std::uint32_t timestamp)
        {
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
    // The muxer emits codec config immediately; Play.Start must precede it.
    if (rtmp_server_start(rtmp_context_, 0, nullptr) != 0 || !play_->startup())
    {
        return -1;
    }
    idle_timer_.stop();
    spdlog::info("rtmp play {}", *target);
    return RTMP_SERVER_ASYNC_START;
}

int rtmp_session::on_publish(std::string_view app, std::string_view stream)
{
    if (publish_ || play_)
    {
        return -1;
    }

    const auto target = parse_rtmp_target(app, stream);
    if (!target)
    {
        return -1;
    }

    // 回调返回 -1 会结束读循环，失败时 publish_ 由 safe_shutdown 清理。
    publish_ = std::make_unique<rtmp_publish_session>(worker_, *target);
    if (!publish_->startup())
    {
        return -1;
    }

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
    idle_timer_.stop();
    if (publish_)
    {
        publish_->shutdown();
        publish_.reset();
    }
    else if (play_)
    {
        play_->shutdown();
        play_.reset();
    }
    transport_->shutdown();
}

}    // namespace media_server
