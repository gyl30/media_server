#include <cstddef>
#include <utility>

#include <spdlog/spdlog.h>
#include <boost/asio/post.hpp>

#include "media/gb28181/gb28181_tcp_session.h"

namespace media_server
{
gb28181_tcp_session::gb28181_tcp_session(
    boost::asio::ip::tcp::socket socket, std::string stream_name, std::uint8_t payload_type, std::uint32_t expected_ssrc)
    : executor_(socket.get_executor()),
      media_(executor_, std::move(stream_name), payload_type, expected_ssrc),
      connection_(std::make_shared<tcp_connection>(std::move(socket)))
{
}

bool gb28181_tcp_session::startup()
{
    if (closed_ || !connection_ || !media_.startup())
    {
        return false;
    }

    input_buffer_.reserve(64 * 1024);
    const auto self = shared_from_this();
    connection_->startup(
        [self](boost::system::error_code error, std::span<const std::uint8_t> data)
        {
            if (error)
            {
                self->shutdown();
                return;
            }
            self->on_read(data);
        },
        [self](boost::system::error_code error, std::size_t)
        {
            if (error)
            {
                self->shutdown();
            }
        });
    spdlog::info("gb28181 tcp session started stream {}", media_.stream_name());
    return true;
}

void gb28181_tcp_session::shutdown()
{
    const auto self = shared_from_this();
    boost::asio::post(executor_, [self]() { self->safe_shutdown(); });
}

const std::string& gb28181_tcp_session::stream_name() const noexcept { return media_.stream_name(); }

void gb28181_tcp_session::on_read(std::span<const std::uint8_t> data)
{
    if (closed_ || data.empty())
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
    media_.shutdown();
    if (connection_)
    {
        connection_->shutdown();
        connection_.reset();
    }
    input_buffer_.clear();
    spdlog::debug("gb28181 tcp session shutdown {}", media_.stream_name());
}

}    // namespace media_server
