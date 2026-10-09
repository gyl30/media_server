#include <span>
#include <vector>
#include <cstddef>
#include <utility>

#include <spdlog/spdlog.h>
#include <boost/asio/post.hpp>
#include <boost/asio/error.hpp>

#include "media/net/worker_context.h"
#include "media/gb28181/gb28181_tcp_receiver_session.h"

namespace media_server
{
gb28181_tcp_receiver_session::gb28181_tcp_receiver_session(worker_context& worker,
                                                           std::string stream_name,
                                                           std::uint8_t payload_type,
                                                           std::uint32_t ssrc)
    : worker_(worker),
      receiver_(worker_, std::move(stream_name), payload_type, ssrc, [this]() { idle_timer_.touch(); }),
      socket_(worker_.io()),
      idle_timer_(worker_.io())
{
}

bool gb28181_tcp_receiver_session::startup(boost::asio::ip::tcp::endpoint remote_endpoint)
{
    if (!receiver_.startup())
    {
        spdlog::error("gb28181 tcp receiver startup failed stream {}", receiver_.stream_name());
        return false;
    }

    const auto self = shared_from_this();
    worker_.spawn([self, remote_endpoint = std::move(remote_endpoint)](boost::asio::yield_context yield) mutable
                  { self->run(std::move(remote_endpoint), yield); });
    return true;
}

bool gb28181_tcp_receiver_session::startup(boost::asio::ip::address bind_address, std::uint16_t listen_port)
{
    if (!receiver_.startup())
    {
        spdlog::error("gb28181 tcp receiver startup failed stream {}", receiver_.stream_name());
        return false;
    }

    listener_ = std::make_unique<tcp_listener>(worker_.io(), listen_port, std::move(bind_address));
    boost::system::error_code error;
    listener_->startup(error);
    if (error)
    {
        spdlog::error("gb28181 tcp listener startup failed stream {} error {}", receiver_.stream_name(), error.message());
        listener_.reset();
        receiver_.shutdown();
        return false;
    }

    const auto self = shared_from_this();
    worker_.spawn([self](boost::asio::yield_context yield) { self->run(std::nullopt, yield); });
    return true;
}

void gb28181_tcp_receiver_session::update_ssrc(std::uint32_t ssrc, std::function<void(bool)> done)
{
    const auto self = shared_from_this();
    boost::asio::post(worker_.io(),
                      [self, ssrc, done = std::move(done)]()
                      {
                          const bool open =
                              self->receiver_.running() && session_registry::instance().receiver_open(self->receiver_.stream_name(), *self);
                          if (open)
                          {
                              self->receiver_.set_expected_ssrc(ssrc);
                          }
                          done(open);
                      });
}

void gb28181_tcp_receiver_session::shutdown()
{
    const auto self = shared_from_this();
    boost::asio::post(worker_.io(), [self]() { self->safe_shutdown(); });
}

void gb28181_tcp_receiver_session::run(std::optional<boost::asio::ip::tcp::endpoint> remote_endpoint, boost::asio::yield_context yield)
{
    idle_timer_.start(weak_from_this(), media_idle_timeout, [weak = weak_from_this()]()
                      {
                          if (const auto owner = weak.lock())
                          {
                              spdlog::info("gb28181 tcp receiver idle timeout {}", owner->receiver_.stream_name());
                              owner->shutdown();
                          }
                      });
    boost::system::error_code error;
    if (remote_endpoint)
    {
        socket_.async_connect(*remote_endpoint, yield[error]);
    }
    else
    {
        listener_->accept(socket_, yield, error);
        listener_.reset();
    }

    if (yield.cancelled() != boost::asio::cancellation_type::none)
    {
        shutdown();
        return;
    }
    if (error)
    {
        if (error != boost::asio::error::operation_aborted)
        {
            spdlog::warn("gb28181 tcp establishment failed stream {} error {}", receiver_.stream_name(), error.message());
        }
        shutdown();
        return;
    }

    transport_ = std::make_shared<tcp_transport>(std::move(socket_));
    spdlog::info("gb28181 tcp session started stream {}", receiver_.stream_name());
    run_read(yield);
}

void gb28181_tcp_receiver_session::run_read(boost::asio::yield_context yield)
{
    boost::system::error_code error;
    std::vector<std::uint8_t> buffer(64 * 1024);
    std::vector<std::uint8_t> input_buffer;
    input_buffer.reserve(buffer.size());
    for (;;)
    {
        const auto read_bytes = transport_->read(buffer, yield, error);
        if (error)
        {
            shutdown();
            return;
        }

        input_buffer.insert(input_buffer.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(read_bytes));
        std::size_t offset = 0;
        while (input_buffer.size() - offset >= 2U)
        {
            const auto packet_bytes = static_cast<std::size_t>((static_cast<std::uint16_t>(input_buffer[offset]) << 8U) | input_buffer[offset + 1U]);
            if (input_buffer.size() - offset < packet_bytes + 2U)
            {
                break;
            }
            offset += 2U;
            if (packet_bytes != 0U)
            {
                const std::span packet{input_buffer.data() + offset, packet_bytes};
                const auto result = receiver_.receive_rtp(packet);
                if (result == gb28181_rtp_receive_result::fatal)
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
}

void gb28181_tcp_receiver_session::safe_shutdown()
{
    idle_timer_.stop();
    if (listener_)
    {
        listener_->shutdown();
    }
    if (transport_)
    {
        transport_->shutdown();
    }
    else
    {
        boost::system::error_code error;
        socket_.close(error);
    }
    receiver_.shutdown();
    // tcp_transport 的关闭投递到同一 worker；排在其后移除槽位，删除请求据此确认 socket 已关闭。
    const auto self = shared_from_this();
    boost::asio::post(worker_.io(), [self]() { session_registry::instance().remove_receiver_session(self->receiver_.stream_name(), *self); });
    spdlog::debug("gb28181 tcp session shutdown {}", receiver_.stream_name());
}

}    // namespace media_server
