#include <array>
#include <cstddef>
#include <span>
#include <utility>
#include <vector>

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
        if (error == boost::asio::error::operation_aborted && socket_.is_open())
        {
            error = boost::asio::error::timed_out;
        }
    }

    if (error)
    {
        if (error != boost::asio::error::operation_aborted)
        {
            spdlog::warn("gb28181 tcp establishment failed stream {} error {}", stream_name_, error.message());
        }
        shutdown();
        return;
    }

    transport_ = std::make_unique<tcp_yield_transport>(std::move(socket_));
    if (!media_.startup())
    {
        spdlog::error("gb28181 tcp input media startup failed stream {}", stream_name_);
        shutdown();
        return;
    }

    spdlog::info("gb28181 tcp session started stream {}", stream_name_);

    std::array<std::uint8_t, 64 * 1024> buffer{};
    std::vector<std::uint8_t> input_buffer;
    input_buffer.reserve(buffer.size());
    for (;;)
    {
        const auto read_bytes = transport_->read(buffer, yield, error);
        if (error)
        {
            break;
        }

        input_buffer.insert(input_buffer.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(read_bytes));
        std::size_t offset = 0;
        while (input_buffer.size() - offset >= 2U)
        {
            const auto packet_bytes =
                static_cast<std::size_t>((static_cast<std::uint16_t>(input_buffer[offset]) << 8U) | input_buffer[offset + 1U]);
            if (input_buffer.size() - offset < packet_bytes + 2U)
            {
                break;
            }
            offset += 2U;
            if (packet_bytes != 0U)
            {
                const std::span packet{input_buffer.data() + offset, packet_bytes};
                if (media_.input_rtp(packet) == gb28181_rtp_input_result::fatal)
                {
                    shutdown();
                    return;
                }
            }
            offset += packet_bytes;
        }

        if (offset == input_buffer.size())
        {
            input_buffer.clear();
        }
        else if (offset != 0U)
        {
            input_buffer.erase(input_buffer.begin(), input_buffer.begin() + static_cast<std::ptrdiff_t>(offset));
        }
    }

    shutdown();
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
    if (transport_)
    {
        transport_->shutdown();
    }
    spdlog::debug("gb28181 tcp session shutdown {}", stream_name_);
}

}    // namespace media_server
