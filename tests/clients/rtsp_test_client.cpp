#include "tests/clients/rtsp_test_client.h"

#include <array>
#include <utility>

#include <boost/asio/connect.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>

extern "C"
{
#include "rtsp-client.h"
#include "rtsp-header-transport.h"
}

namespace media_server::test
{

rtsp_test_client::rtsp_test_client(boost::asio::any_io_executor executor, std::string path)
    : resolver_(executor), socket_(executor), path_(std::move(path))
{
}

rtsp_test_client::~rtsp_test_client()
{
    rtsp_client_destroy(client_);
}

boost::asio::awaitable<boost::system::error_code> rtsp_test_client::publish(
    std::string host, std::uint16_t port, std::string sdp, std::vector<std::uint8_t> rtp)
{
    sdp_ = std::move(sdp);
    auto error = co_await start(std::move(host), port, mode::publish);
    if (error)
    {
        co_return error;
    }

    std::vector<std::uint8_t> interleaved{0x24, 0x00, static_cast<std::uint8_t>(rtp.size() >> 8U), static_cast<std::uint8_t>(rtp.size())};
    interleaved.insert(interleaved.end(), rtp.begin(), rtp.end());
    co_await boost::asio::async_write(socket_, boost::asio::buffer(interleaved), boost::asio::redirect_error(boost::asio::use_awaitable, error));
    co_return error;
}

boost::asio::awaitable<boost::system::error_code> rtsp_test_client::play(std::string host, std::uint16_t port)
{
    auto error = co_await start(std::move(host), port, mode::play);
    if (error)
    {
        co_return error;
    }

    std::array<std::uint8_t, 8192> read_buffer{};
    while (rtp_.empty())
    {
        const auto bytes = co_await socket_.async_read_some(boost::asio::buffer(read_buffer), boost::asio::redirect_error(boost::asio::use_awaitable, error));
        if (error || rtsp_client_input(client_, read_buffer.data(), bytes) != 0)
        {
            co_return error ? error : boost::asio::error::operation_aborted;
        }
    }
    co_return boost::system::error_code{};
}

boost::asio::awaitable<boost::system::error_code> rtsp_test_client::start(std::string host, std::uint16_t port, mode operation)
{
    boost::system::error_code error;
    mode_ = operation;
    completed_ = false;
    uri_ = "rtsp://" + host + ':' + std::to_string(port) + path_;
    const auto endpoints = co_await resolver_.async_resolve(host, std::to_string(port), boost::asio::redirect_error(boost::asio::use_awaitable, error));
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
    handler.send = &rtsp_test_client::send_callback;
    handler.rtpport = &rtsp_test_client::rtp_port_callback;
    handler.onannounce = &rtsp_test_client::announce_callback;
    handler.ondescribe = &rtsp_test_client::describe_callback;
    handler.onsetup = &rtsp_test_client::setup_callback;
    handler.onplay = &rtsp_test_client::play_callback;
    handler.onrecord = &rtsp_test_client::record_callback;
    handler.onpause = &rtsp_test_client::ignore_callback;
    handler.onteardown = &rtsp_test_client::ignore_callback;
    handler.onrtp = &rtsp_test_client::rtp_callback;
    client_ = rtsp_client_create(uri_.c_str(), nullptr, nullptr, &handler, this);
    if (client_ == nullptr || (mode_ == mode::publish ? rtsp_client_announce(client_, sdp_.c_str()) : rtsp_client_describe(client_)) != 0)
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
        const auto bytes = co_await socket_.async_read_some(boost::asio::buffer(read_buffer), boost::asio::redirect_error(boost::asio::use_awaitable, error));
        if (error || rtsp_client_input(client_, read_buffer.data(), bytes) != 0)
        {
            co_return error ? error : boost::asio::error::operation_aborted;
        }
    }
    co_return co_await flush();
}

int rtsp_test_client::send_callback(void* param, const char*, const void* request, std::size_t bytes)
{
    auto* self = static_cast<rtsp_test_client*>(param);
    if (bytes != 0)
    {
        const auto* data = static_cast<const std::uint8_t*>(request);
        self->writes_.emplace_back(data, data + bytes);
    }
    return static_cast<int>(bytes);
}

int rtsp_test_client::rtp_port_callback(void*, int media, const char*, unsigned short port[2], char*, int)
{
    port[0] = static_cast<unsigned short>(media * 2);
    port[1] = static_cast<unsigned short>(port[0] + 1U);
    return RTSP_TRANSPORT_RTP_TCP;
}

int rtsp_test_client::announce_callback(void* param)
{
    auto* self = static_cast<rtsp_test_client*>(param);
    return rtsp_client_setup(self->client_, self->sdp_.c_str(), static_cast<int>(self->sdp_.size()));
}

int rtsp_test_client::describe_callback(void* param, const char* sdp, int len)
{
    auto* self = static_cast<rtsp_test_client*>(param);
    return rtsp_client_setup(self->client_, sdp, len);
}

int rtsp_test_client::setup_callback(void* param, int, std::int64_t)
{
    auto* self = static_cast<rtsp_test_client*>(param);
    if (self->mode_ == mode::publish)
    {
        return rtsp_client_record(self->client_, nullptr, nullptr);
    }

    const auto result = rtsp_client_play(self->client_, nullptr, nullptr);
    self->completed_ = result == 0;
    return result;
}

int rtsp_test_client::play_callback(void* param,
                                    int,
                                    const std::uint64_t*,
                                    const std::uint64_t*,
                                    const double*,
                                    const ::rtsp_rtp_info_t*,
                                    int)
{
    static_cast<rtsp_test_client*>(param)->completed_ = true;
    return 0;
}

int rtsp_test_client::record_callback(void* param,
                                      int,
                                      const std::uint64_t*,
                                      const std::uint64_t*,
                                      const double*,
                                      const ::rtsp_rtp_info_t*,
                                      int)
{
    static_cast<rtsp_test_client*>(param)->completed_ = true;
    return 0;
}

int rtsp_test_client::ignore_callback(void*)
{
    return 0;
}

void rtsp_test_client::rtp_callback(void* param, std::uint8_t channel, const void* data, std::uint16_t bytes)
{
    if ((channel % 2U) == 0U && bytes != 0)
    {
        const auto* rtp = static_cast<const std::uint8_t*>(data);
        static_cast<rtsp_test_client*>(param)->rtp_.assign(rtp, rtp + bytes);
    }
}

boost::asio::awaitable<boost::system::error_code> rtsp_test_client::flush()
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

}    // namespace media_server::test
