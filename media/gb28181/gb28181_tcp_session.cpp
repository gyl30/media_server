#include <cstddef>
#include <utility>

#include <spdlog/spdlog.h>
#include <boost/asio/cancel_after.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/post.hpp>

#include "media/core/stream_registry.h"
#include "media/net/worker_context.h"
#include "media/gb28181/gb28181_tcp_session.h"

namespace media_server
{
gb28181_tcp_session::gb28181_tcp_session(worker_context& worker,
                                         std::string stream_name,
                                         gb28181_description description,
                                         std::chrono::milliseconds establishment_timeout)
    : worker_(worker),
      stream_name_(std::move(stream_name)),
      description_(std::move(description)),
      media_(worker_, stream_name_, description_.payload_type, description_.ssrc),
      establishment_timeout_(establishment_timeout),
      socket_(worker_.io())
{
}

bool gb28181_tcp_session::startup()
{
    if (closed_)
    {
        return false;
    }

    if (description_.transport == gb28181_transport::tcp_passive)
    {
        listener_ = std::make_unique<tcp_listener>(worker_.io(), description_.rtp_port, description_.address);
        boost::system::error_code error;
        listener_->startup(error);
        if (error)
        {
            spdlog::error("gb28181 tcp listener startup failed stream {} error {}", stream_name_, error.message());
            listener_.reset();
            return false;
        }
    }

    const auto self = shared_from_this();
    boost::asio::spawn(worker_.io(), [self](boost::asio::yield_context yield) { self->run(yield); }, boost::asio::detached);
    return true;
}

void gb28181_tcp_session::shutdown()
{
    const auto self = shared_from_this();
    boost::asio::post(worker_.io(), [self]() { self->safe_shutdown(); });
}

const std::string& gb28181_tcp_session::stream_name() const noexcept { return stream_name_; }

void gb28181_tcp_session::run(boost::asio::yield_context yield)
{
    if (closed_)
    {
        return;
    }

    boost::system::error_code error;
    if (description_.transport == gb28181_transport::tcp_passive)
    {
        listener_->accept(socket_, establishment_timeout_, yield, error);
        listener_->shutdown();
        listener_.reset();
    }
    else
    {
        socket_.async_connect(boost::asio::ip::tcp::endpoint{description_.address, description_.rtp_port},
                              boost::asio::cancel_after(establishment_timeout_, yield[error]));
        if (error == boost::asio::error::operation_aborted && !closed_)
        {
            error = boost::asio::error::timed_out;
        }
    }

    if (error || closed_)
    {
        if (error && !closed_)
        {
            spdlog::warn("gb28181 tcp establishment failed stream {} error {}", stream_name_, error.message());
        }
        safe_shutdown();
        return;
    }

    if (!media_.startup())
    {
        spdlog::error("gb28181 tcp input media startup failed stream {}", stream_name_);
        safe_shutdown();
        return;
    }

    connection_ = std::make_shared<tcp_connection>(std::move(socket_));
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
    registry::instance().remove_input_session(stream_name_, *this);
    if (listener_)
    {
        listener_->shutdown();
    }
    boost::system::error_code error;
    socket_.cancel(error);
    socket_.close(error);
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
