#include <array>
#include <memory>
#include <utility>

#include <boost/asio/write.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/redirect_error.hpp>

#include "bench/clients/rtmp_client.h"

extern "C"
{
#include "flv-proto.h"
#include "flv-header.h"
#include "rtmp-client.h"
}

namespace media_server::bench
{

rtmp_client::rtmp_client(boost::asio::io_context& io, std::string path) : resolver_(io), socket_(io)
{
    const auto slash = path.find('/');
    if (slash == std::string::npos)
    {
        stream_ = std::move(path);
    }
    else
    {
        app_ = path.substr(0, slash);
        stream_ = path.substr(slash + 1U);
    }
}

rtmp_client::~rtmp_client()
{
    if (client_ != nullptr)
    {
        rtmp_client_destroy(client_);
    }
}

boost::asio::awaitable<boost::system::error_code> rtmp_client::play(std::string host, std::uint16_t port, bool expect_audio)
{
    boost::system::error_code error;
    const auto endpoints =
        co_await resolver_.async_resolve(host, std::to_string(port), boost::asio::redirect_error(boost::asio::use_awaitable, error));
    if (error)
    {
        co_return error;
    }
    co_await boost::asio::async_connect(socket_, endpoints, boost::asio::redirect_error(boost::asio::use_awaitable, error));
    if (error)
    {
        co_return error;
    }

    rtmp_client_handler_t handler{};
    handler.send = &rtmp_client::send_callback;
    handler.onvideo = &rtmp_client::video_callback;
    handler.onaudio = &rtmp_client::audio_callback;
    handler.onscript = &rtmp_client::ignore_callback;
    const auto tc_url = "rtmp://" + host + ':' + std::to_string(port) + '/' + app_;
    client_ = rtmp_client_create(app_.c_str(), stream_.c_str(), tc_url.c_str(), this, &handler);
    if (client_ == nullptr || rtmp_client_start(client_, 2) != 0)
    {
        co_return boost::asio::error::operation_aborted;
    }

    std::array<std::uint8_t, 8192> read_buffer{};
    while (received_messages_ == 0 || (expect_audio && received_audio_messages_ == 0))
    {
        error = co_await flush();
        if (error)
        {
            co_return error;
        }
        const auto bytes =
            co_await socket_.async_read_some(boost::asio::buffer(read_buffer), boost::asio::redirect_error(boost::asio::use_awaitable, error));
        if (error || rtmp_client_input(client_, read_buffer.data(), bytes) != 0)
        {
            co_return error ? error : boost::asio::error::operation_aborted;
        }
    }
    co_return boost::system::error_code{};
}

boost::asio::awaitable<boost::system::error_code> rtmp_client::consume_one()
{
    boost::system::error_code error = co_await flush();
    if (error)
    {
        co_return error;
    }

    std::array<std::uint8_t, 8192> read_buffer{};
    const auto bytes =
        co_await socket_.async_read_some(boost::asio::buffer(read_buffer), boost::asio::redirect_error(boost::asio::use_awaitable, error));
    if (error)
    {
        co_return error;
    }
    if (rtmp_client_input(client_, read_buffer.data(), bytes) != 0)
    {
        co_return boost::asio::error::operation_aborted;
    }
    co_return boost::system::error_code{};
}

void rtmp_client::cancel() noexcept
{
    boost::system::error_code error;
    resolver_.cancel();
    socket_.cancel(error);
}

bool rtmp_client::handshake_complete() const noexcept
{
    constexpr int media_state = 4;
    return client_ != nullptr && rtmp_client_getstate(client_) == media_state;
}

int rtmp_client::send_callback(void* param, const void* header, std::size_t header_bytes, const void* payload, std::size_t payload_bytes)
{
    auto* self = static_cast<rtmp_client*>(param);
    self->writes_.emplace_back();
    auto& write = self->writes_.back();
    if (header_bytes != 0)
    {
        const auto* header_data = static_cast<const std::uint8_t*>(header);
        write.insert(write.end(), header_data, header_data + header_bytes);
    }
    if (payload_bytes != 0)
    {
        const auto* payload_data = static_cast<const std::uint8_t*>(payload);
        write.insert(write.end(), payload_data, payload_data + payload_bytes);
    }
    return static_cast<int>(header_bytes + payload_bytes);
}

int rtmp_client::video_callback(void* param, const void* data, std::size_t bytes, std::uint32_t timestamp)
{
    flv_video_tag_header_t header{};
    if (flv_video_tag_header_read(&header, static_cast<const std::uint8_t*>(data), bytes) < 0)
    {
        return -1;
    }
    if (header.avpacket != FLV_AVPACKET)
    {
        return 0;
    }
    auto* self = static_cast<rtmp_client*>(param);
    if (!self->video_timestamps_.observe(timestamp))
    {
        return -1;
    }
    self->received_bytes_ += bytes;
    ++self->received_messages_;
    return 0;
}

int rtmp_client::audio_callback(void* param, const void* data, std::size_t bytes, std::uint32_t timestamp)
{
    flv_audio_tag_header_t header{};
    if (flv_audio_tag_header_read(&header, static_cast<const std::uint8_t*>(data), bytes) < 0)
    {
        return -1;
    }
    if (header.avpacket != FLV_AVPACKET)
    {
        return 0;
    }
    auto* self = static_cast<rtmp_client*>(param);
    if (!self->audio_timestamps_.observe(timestamp))
    {
        return -1;
    }
    self->received_audio_bytes_ += bytes;
    ++self->received_audio_messages_;
    return 0;
}

int rtmp_client::ignore_callback(void*, const void*, std::size_t, std::uint32_t) { return 0; }

boost::asio::awaitable<boost::system::error_code> rtmp_client::flush()
{
    boost::system::error_code error;
    while (!writes_.empty())
    {
        const auto& write = writes_.front();
        co_await boost::asio::async_write(socket_, boost::asio::buffer(write), boost::asio::redirect_error(boost::asio::use_awaitable, error));
        if (error)
        {
            co_return error;
        }
        writes_.erase(writes_.begin());
    }
    co_return boost::system::error_code{};
}

}    // namespace media_server::bench
