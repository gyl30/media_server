#include <cstddef>
#include <utility>

#include <spdlog/spdlog.h>
#include <boost/asio/post.hpp>

#include "media/gb28181/gb28181_tcp_session.h"
#include "media/core/stream_registry.h"

namespace media_server
{
gb28181_tcp_session::gb28181_tcp_session(
    boost::asio::any_io_executor executor,
    std::shared_ptr<tcp_socket_source> socket_source,
    std::string stream_name,
    std::uint8_t payload_type,
    std::uint32_t expected_ssrc)
    : executor_(std::move(executor)),
      socket_source_(std::move(socket_source)),
      stream_name_(std::move(stream_name)),
      media_(executor_, stream_name_, payload_type, expected_ssrc)
{
}

bool gb28181_tcp_session::startup()
{
    if (closed_ || shutdown_requested_.load() || !socket_source_)
    {
        return false;
    }

    const auto weak = weak_from_this();
    const auto error = socket_source_->startup(
        [weak](boost::system::error_code source_error, boost::asio::ip::tcp::socket socket) mutable
        {
            if (const auto self = weak.lock())
            {
                self->on_socket(source_error, std::move(socket));
                return;
            }
            boost::system::error_code ignored;
            socket.close(ignored);
        });
    if (error)
    {
        spdlog::error("gb28181 tcp source startup failed stream {} error {}", stream_name_, error.message());
        shutdown();
        return false;
    }
    return true;
}

void gb28181_tcp_session::shutdown()
{
    if (shutdown_requested_.exchange(true))
    {
        return;
    }
    const auto self = shared_from_this();
    boost::asio::post(executor_, [self]() { self->safe_shutdown(); });
}

const std::string& gb28181_tcp_session::stream_name() const noexcept { return stream_name_; }

void gb28181_tcp_session::on_socket(boost::system::error_code error, boost::asio::ip::tcp::socket socket)
{
    if (error)
    {
        spdlog::warn("gb28181 tcp source failed stream {} error {}", stream_name_, error.message());
        shutdown();
        return;
    }
    if (shutdown_requested_.load() || closed_)
    {
        boost::system::error_code ignored;
        socket.close(ignored);
        return;
    }

    socket_source_.reset();
    if (!media_.startup())
    {
        spdlog::error("gb28181 tcp input media startup failed stream {}", stream_name_);
        shutdown();
        return;
    }

    connection_ = std::make_shared<tcp_connection>(std::move(socket));
    input_buffer_.reserve(64 * 1024);
    const auto weak = weak_from_this();
    connection_->startup(
        [weak](boost::system::error_code connection_error, std::span<const std::uint8_t> data)
        {
            if (const auto self = weak.lock())
            {
                if (connection_error)
                {
                    self->shutdown();
                    return;
                }
                self->on_read(data);
            }
        },
        [weak](boost::system::error_code connection_error, std::size_t)
        {
            if (connection_error)
            {
                if (const auto self = weak.lock())
                {
                    self->shutdown();
                }
            }
        });
    spdlog::info("gb28181 tcp session started stream {}", stream_name_);
}

void gb28181_tcp_session::on_read(std::span<const std::uint8_t> data)
{
    if (closed_ || shutdown_requested_.load() || data.empty())
    {
        return;
    }

    input_buffer_.insert(input_buffer_.end(), data.begin(), data.end());
    std::size_t offset = 0;
    while (input_buffer_.size() - offset >= 2)
    {
        const auto bytes = static_cast<std::size_t>((static_cast<std::uint16_t>(input_buffer_[offset]) << 8U) | input_buffer_[offset + 1]);
        if (input_buffer_.size() - offset < bytes + 2U)
        {
            break;
        }
        offset += 2;
        if (bytes != 0)
        {
            const std::span packet{input_buffer_.data() + offset, bytes};
            if (media_.input_rtp(packet) == gb28181_rtp_input_result::fatal)
            {
                shutdown();
                return;
            }
        }
        offset += bytes;
    }

    if (offset == input_buffer_.size())
    {
        input_buffer_.clear();
    }
    else if (offset != 0)
    {
        input_buffer_.erase(input_buffer_.begin(), input_buffer_.begin() + static_cast<std::ptrdiff_t>(offset));
    }
}

void gb28181_tcp_session::safe_shutdown()
{
    if (closed_)
    {
        return;
    }
    closed_ = true;
    registry::instance().remove_input_session(stream_name_, *this);
    if (socket_source_)
    {
        socket_source_->shutdown();
        socket_source_.reset();
    }
    media_.shutdown();
    if (connection_)
    {
        connection_->shutdown();
        connection_.reset();
    }
    input_buffer_.clear();
    spdlog::debug("gb28181 tcp session shutdown {}", stream_name_);
}

}    // namespace media_server
