#include "tests/clients/rtmp_test_client.h"

#include <array>
#include <utility>

#include <boost/asio/redirect_error.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>

extern "C"
{
#include "rtmp-client.h"
#include "rtmp-internal.h"
}

namespace media_server::test
{

rtmp_test_client::rtmp_test_client(boost::asio::any_io_executor executor, std::string app, std::string stream)
    : resolver_(executor), socket_(executor), app_(std::move(app)), stream_(std::move(stream))
{
}

rtmp_test_client::~rtmp_test_client()
{
    rtmp_client_destroy(client_);
}

boost::asio::awaitable<boost::system::error_code> rtmp_test_client::publish(
    std::string host, std::uint16_t port, std::vector<std::uint8_t> metadata, std::vector<std::uint8_t> video_config)
{
    boost::system::error_code error;
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

    rtmp_client_handler_t handler{};
    handler.send = &rtmp_test_client::send_callback;
    const auto tc_url = "rtmp://" + host + ':' + std::to_string(port) + '/' + app_;
    client_ = rtmp_client_create(app_.c_str(), stream_.c_str(), tc_url.c_str(), this, &handler);
    if (client_ == nullptr || rtmp_client_start(client_, 0) != 0)
    {
        co_return boost::asio::error::operation_aborted;
    }

    std::array<std::uint8_t, 8192> read_buffer{};
    while (rtmp_client_getstate(client_) != RTMP_STATE_START)
    {
        error = co_await flush();
        if (error)
        {
            co_return error;
        }
        const auto bytes = co_await socket_.async_read_some(boost::asio::buffer(read_buffer), boost::asio::redirect_error(boost::asio::use_awaitable, error));
        if (error || rtmp_client_input(client_, read_buffer.data(), bytes) != 0)
        {
            co_return error ? error : boost::asio::error::operation_aborted;
        }
    }

    if (rtmp_client_push_script(client_, metadata.data(), metadata.size(), 0) != 0 ||
        rtmp_client_push_video(client_, video_config.data(), video_config.size(), 0) != 0)
    {
        co_return boost::asio::error::operation_aborted;
    }
    co_return co_await flush();
}

boost::asio::awaitable<boost::system::error_code> rtmp_test_client::play(std::string host, std::uint16_t port)
{
    boost::system::error_code error;
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

    rtmp_client_handler_t handler{};
    handler.send = &rtmp_test_client::send_callback;
    handler.onvideo = &rtmp_test_client::video_callback;
    handler.onaudio = &rtmp_test_client::ignore_callback;
    handler.onscript = &rtmp_test_client::ignore_callback;
    const auto tc_url = "rtmp://" + host + ':' + std::to_string(port) + '/' + app_;
    client_ = rtmp_client_create(app_.c_str(), stream_.c_str(), tc_url.c_str(), this, &handler);
    if (client_ == nullptr || rtmp_client_start(client_, 2) != 0)
    {
        co_return boost::asio::error::operation_aborted;
    }

    std::array<std::uint8_t, 8192> read_buffer{};
    while (video_.empty())
    {
        error = co_await flush();
        if (error)
        {
            co_return error;
        }
        const auto bytes = co_await socket_.async_read_some(boost::asio::buffer(read_buffer), boost::asio::redirect_error(boost::asio::use_awaitable, error));
        if (error || rtmp_client_input(client_, read_buffer.data(), bytes) != 0)
        {
            co_return error ? error : boost::asio::error::operation_aborted;
        }
    }
    co_return boost::system::error_code{};
}

int rtmp_test_client::send_callback(void* param, const void* header, std::size_t header_bytes, const void* payload, std::size_t payload_bytes)
{
    auto* self = static_cast<rtmp_test_client*>(param);
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

int rtmp_test_client::video_callback(void* param, const void* data, std::size_t bytes, std::uint32_t)
{
    auto* self = static_cast<rtmp_test_client*>(param);
    self->video_.assign(static_cast<const std::uint8_t*>(data), static_cast<const std::uint8_t*>(data) + bytes);
    return 0;
}

int rtmp_test_client::ignore_callback(void*, const void*, std::size_t, std::uint32_t)
{
    return 0;
}

boost::asio::awaitable<boost::system::error_code> rtmp_test_client::flush()
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
