#include <span>
#include <vector>
#include <utility>

#include <spdlog/spdlog.h>
#include <boost/asio/post.hpp>

#include "media/net/worker_context.h"
#include "media/rtmp/rtmp_pull_session.h"
#include "media/rtmp/rtmp_publish_session.h"

extern "C"
{
#include "rtmp-client.h"
#include "rtmp-internal.h"
}

namespace media_server
{

namespace
{
// rtmp_client_start 的 publish 参数：1 表示 Play（直播或点播）。
constexpr int rtmp_client_play = 1;
}    // namespace

rtmp_pull_session::rtmp_pull_session(worker_context& worker, rtmp_pull_config config)
    : worker_(worker), config_(std::move(config)), resolver_(worker_.io()), connect_socket_(worker_.io()), idle_timer_(worker_.io())
{
}

rtmp_pull_session::~rtmp_pull_session() = default;

void rtmp_pull_session::startup()
{
    const auto self = shared_from_this();
    // 连接、握手和收流都受同一空闲时间约束，上游不再发送数据时结束会话。
    idle_timer_.start(self,
                      [this]()
                      {
                          spdlog::info("rtmp pull idle timeout {}", config_.stream_id);
                          shutdown();
                      });
    worker_.spawn([self](boost::asio::yield_context yield) { self->run(yield); });
}

void rtmp_pull_session::shutdown()
{
    const auto self = shared_from_this();
    boost::asio::post(worker_.io(), [self]() { self->safe_shutdown(); });
}

void rtmp_pull_session::safe_shutdown()
{
    idle_timer_.stop();
    session_registry::instance().remove_receiver_session(config_.stream_id, *this);
    if (media_)
    {
        media_->shutdown();
        media_.reset();
    }
    resolver_.cancel();
    boost::system::error_code error;
    connect_socket_.close(error);
    if (transport_)
    {
        transport_->shutdown();
    }
    spdlog::debug("rtmp pull shutdown {}", config_.stream_id);
}

int rtmp_pull_session::send_callback(void* param, const void* header, std::size_t header_bytes, const void* payload, std::size_t payload_bytes)
{
    auto* self = static_cast<rtmp_pull_session*>(param);
    if (!self->transport_)
    {
        return -1;
    }
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

// 父清理先于 TCP 子关闭完成，已读到的数据仍可能进入回调；media_ 为空时结束读循环。
int rtmp_pull_session::video_callback(void* param, const void* data, std::size_t bytes, std::uint32_t timestamp)
{
    auto* self = static_cast<rtmp_pull_session*>(param);
    return self->media_ ? self->media_->on_video(data, bytes, timestamp) : -1;
}

int rtmp_pull_session::audio_callback(void* param, const void* data, std::size_t bytes, std::uint32_t timestamp)
{
    auto* self = static_cast<rtmp_pull_session*>(param);
    return self->media_ ? self->media_->on_audio(data, bytes, timestamp) : -1;
}

int rtmp_pull_session::script_callback(void* param, const void* data, std::size_t bytes, std::uint32_t)
{
    auto* self = static_cast<rtmp_pull_session*>(param);
    return self->media_ ? self->media_->on_script(std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(data), bytes)) : -1;
}

void rtmp_pull_session::run(boost::asio::yield_context yield)
{
    boost::system::error_code error;
    const auto endpoints = resolver_.async_resolve(config_.host, std::to_string(config_.port), yield[error]);
    if (yield.cancelled() != boost::asio::cancellation_type::none ||
        session_registry::instance().find_receiver_session(config_.stream_id).get() != this || error)
    {
        shutdown();
        return;
    }

    boost::asio::async_connect(connect_socket_, endpoints, yield[error]);
    if (yield.cancelled() != boost::asio::cancellation_type::none ||
        session_registry::instance().find_receiver_session(config_.stream_id).get() != this || error)
    {
        shutdown();
        return;
    }

    transport_ = std::make_shared<tcp_transport>(std::move(connect_socket_));
    const auto self = shared_from_this();
    transport_->set_write_callback(
        [self](boost::system::error_code write_error, std::size_t)
        {
            if (write_error)
            {
                self->shutdown();
            }
        });
    media_ = std::make_unique<rtmp_publish_session>(worker_, config_.stream_id);
    if (!media_->startup())
    {
        shutdown();
        return;
    }

    rtmp_client_handler_t handler{};
    handler.send = &rtmp_pull_session::send_callback;
    handler.onvideo = &rtmp_pull_session::video_callback;
    handler.onaudio = &rtmp_pull_session::audio_callback;
    handler.onscript = &rtmp_pull_session::script_callback;
    auto* client = rtmp_client_create(config_.app.c_str(), config_.stream.c_str(), config_.tc_url.c_str(), this, &handler);
    if (client == nullptr)
    {
        shutdown();
        return;
    }

    spdlog::info("rtmp pull connected stream {}", config_.stream_id);
    if (rtmp_client_start(client, rtmp_client_play) == 0)
    {
        run_read(client, yield);
    }
    rtmp_client_destroy(client);
    shutdown();
}

void rtmp_pull_session::run_read(rtmp_client_t* client, boost::asio::yield_context yield)
{
    boost::system::error_code error;
    std::vector<std::uint8_t> buffer(64 * 1024);
    for (;;)
    {
        const auto bytes = transport_->read(buffer, yield, error);
        if (error)
        {
            return;
        }
        idle_timer_.touch();
        // 上游拒绝（StreamNotFound、Play.Failed 等）时 input 返回错误；Play.Stop / Complete 后状态为 STOP。
        if (rtmp_client_input(client, buffer.data(), bytes) != 0 || rtmp_client_getstate(client) == RTMP_STATE_STOP)
        {
            spdlog::info("rtmp pull upstream ended {}", config_.stream_id);
            return;
        }
    }
}

}    // namespace media_server
