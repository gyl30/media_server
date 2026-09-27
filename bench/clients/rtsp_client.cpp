#include <array>
#include <utility>

#include <boost/asio/write.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/redirect_error.hpp>

#include "bench/clients/rtsp_client.h"

extern "C"
{
#include "rtsp-client.h"
#include "rtsp-header-transport.h"
}

namespace media_server::bench
{

rtsp_client::rtsp_client(boost::asio::io_context& io, std::string path) : resolver_(io), socket_(io), path_(std::move(path)) {}

rtsp_client::~rtsp_client() { rtsp_client_destroy(client_); }

boost::asio::awaitable<boost::system::error_code> rtsp_client::play(std::string host, std::uint16_t port)
{
    received_channels_.clear();
    received_bytes_ = 0;
    received_messages_ = 0;
    received_audio_bytes_ = 0;
    received_audio_messages_ = 0;
    auto error = co_await start(std::move(host), port);
    if (error)
    {
        co_return error;
    }

    std::array<std::uint8_t, 8192> read_buffer{};
    while (received_channels_.size() < 2)
    {
        const auto bytes =
            co_await socket_.async_read_some(boost::asio::buffer(read_buffer), boost::asio::redirect_error(boost::asio::use_awaitable, error));
        if (error || rtsp_client_input(client_, read_buffer.data(), bytes) != 0)
        {
            co_return error ? error : boost::asio::error::operation_aborted;
        }
    }
    co_return boost::system::error_code{};
}

boost::asio::awaitable<boost::system::error_code> rtsp_client::consume_one()
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
    if (rtsp_client_input(client_, read_buffer.data(), bytes) != 0)
    {
        co_return boost::asio::error::operation_aborted;
    }
    co_return boost::system::error_code{};
}

void rtsp_client::cancel() noexcept
{
    boost::system::error_code error;
    socket_.cancel(error);
}

boost::asio::awaitable<boost::system::error_code> rtsp_client::start(std::string host, std::uint16_t port)
{
    boost::system::error_code error;
    completed_ = false;
    uri_ = "rtsp://" + host + ':' + std::to_string(port) + path_;
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

    rtsp_client_handler_t handler{};
    handler.send = &rtsp_client::send_callback;
    handler.rtpport = &rtsp_client::rtp_port_callback;
    handler.ondescribe = &rtsp_client::describe_callback;
    handler.onsetup = &rtsp_client::setup_callback;
    handler.onplay = &rtsp_client::play_callback;
    handler.onpause = &rtsp_client::ignore_callback;
    handler.onteardown = &rtsp_client::ignore_callback;
    handler.onrtp = &rtsp_client::rtp_callback;
    client_ = rtsp_client_create(uri_.c_str(), nullptr, nullptr, &handler, this);
    if (client_ == nullptr || rtsp_client_describe(client_) != 0)
    {
        co_return boost::asio::error::operation_aborted;
    }

    std::array<std::uint8_t, 8192> read_buffer{};
    while (!completed_)
    {
        error = co_await flush();
        if (error)
        {
            co_return error;
        }
        const auto bytes =
            co_await socket_.async_read_some(boost::asio::buffer(read_buffer), boost::asio::redirect_error(boost::asio::use_awaitable, error));
        if (error || rtsp_client_input(client_, read_buffer.data(), bytes) != 0)
        {
            co_return error ? error : boost::asio::error::operation_aborted;
        }
    }
    co_return co_await flush();
}

int rtsp_client::send_callback(void* param, const char*, const void* request, std::size_t bytes)
{
    auto* self = static_cast<rtsp_client*>(param);
    if (bytes != 0)
    {
        const auto* data = static_cast<const std::uint8_t*>(request);
        self->writes_.emplace_back(data, data + bytes);
    }
    return static_cast<int>(bytes);
}

int rtsp_client::rtp_port_callback(void*, int media, const char*, unsigned short port[2], char*, int)
{
    port[0] = static_cast<unsigned short>(media * 2);
    port[1] = static_cast<unsigned short>(port[0] + 1U);
    return RTSP_TRANSPORT_RTP_TCP;
}

int rtsp_client::describe_callback(void* param, const char* sdp, int len)
{
    auto* self = static_cast<rtsp_client*>(param);
    self->sdp_.assign(sdp, static_cast<std::size_t>(len));
    return rtsp_client_setup(self->client_, self->sdp_.c_str(), static_cast<int>(self->sdp_.size()));
}

int rtsp_client::setup_callback(void* param, int, std::int64_t)
{
    auto* self = static_cast<rtsp_client*>(param);
    const auto result = rtsp_client_play(self->client_, nullptr, nullptr);
    self->completed_ = result == 0;
    return result;
}

int rtsp_client::play_callback(void* param, int, const std::uint64_t*, const std::uint64_t*, const double*, const ::rtsp_rtp_info_t*, int)
{
    static_cast<rtsp_client*>(param)->completed_ = true;
    return 0;
}

int rtsp_client::ignore_callback(void*) { return 0; }

void rtsp_client::rtp_callback(void* param, std::uint8_t channel, const void*, std::uint16_t bytes)
{
    if ((channel % 2U) == 0U && bytes != 0)
    {
        auto* self = static_cast<rtsp_client*>(param);
        // The benchmark fixture advertises video on channel 0 and audio on channel 2.
        if (channel == 0)
        {
            self->received_bytes_ += bytes;
            ++self->received_messages_;
        }
        else
        {
            self->received_audio_bytes_ += bytes;
            ++self->received_audio_messages_;
        }
        self->received_channels_.insert(channel);
    }
}

boost::asio::awaitable<boost::system::error_code> rtsp_client::flush()
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
