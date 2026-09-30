#include <limits>
#include <vector>
#include <utility>
#include <algorithm>

#include <spdlog/spdlog.h>
#include <boost/asio/post.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/detached.hpp>

#include "media/net/worker_context.h"
#include "media/core/stream_registry.h"
#include "media/gb28181/gb28181_rtp_sender.h"
#include "media/gb28181/gb28181_tcp_sender_session.h"

namespace media_server
{
gb28181_tcp_sender_session::gb28181_tcp_sender_session(worker_context& worker,
                                                       std::shared_ptr<media_stream> stream,
                                                       std::string sender_id,
                                                       gb28181_transport_config config,
                                                       boost::asio::ip::address bind_address)
    : worker_(worker),
      stream_(std::move(stream)),
      sender_id_(std::move(sender_id)),
      config_(std::move(config)),
      bind_address_(std::move(bind_address)),
      socket_(worker_.io())
{
}

void gb28181_tcp_sender_session::startup()
{
    if (config_.mode == gb28181_transport::tcp_passive)
    {
        listener_ = std::make_unique<tcp_listener>(worker_.io(), config_.listen_port, bind_address_);
        boost::system::error_code error;
        listener_->startup(error);
        if (error)
        {
            spdlog::error("gb28181 tcp sender listener startup failed stream {} sender {} error {}", stream_->name(), sender_id_, error.message());
            listener_.reset();
            shutdown();
            return;
        }
    }

    const auto self = shared_from_this();
    worker_.spawn([self](boost::asio::yield_context yield) { self->run(yield); });
}

void gb28181_tcp_sender_session::run(boost::asio::yield_context yield)
{
    boost::system::error_code error;
    if (config_.mode == gb28181_transport::tcp_passive)
    {
        listener_->accept(socket_, yield, error);
        listener_->shutdown();
        listener_.reset();
    }
    else
    {
        socket_.async_connect(boost::asio::ip::tcp::endpoint{config_.remote_address, config_.remote_port}, yield[error]);
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
            spdlog::warn("gb28181 tcp sender establishment failed stream {} sender {} error {}", stream_->name(), sender_id_, error.message());
        }
        shutdown();
        return;
    }
    if (!socket_.is_open())
    {
        return;
    }

    transport_ = std::make_shared<tcp_transport>(std::move(socket_));
    const auto self = shared_from_this();
    transport_->set_write_callback(
        [weak = std::weak_ptr<gb28181_tcp_sender_session>(self)](boost::system::error_code error, std::size_t)
        {
            if (error)
            {
                if (const auto owner = weak.lock())
                {
                    owner->shutdown();
                }
            }
        });
    sender_ = std::make_shared<gb28181_rtp_sender>(
        worker_,
        stream_,
        config_.payload_type,
        config_.ssrc,
        [self](std::vector<std::uint8_t> packet) { self->send_packet(std::move(packet)); },
        [self]() { self->shutdown(); });
    if (!sender_->startup())
    {
        shutdown();
        return;
    }

    spdlog::info("gb28181 tcp sender started stream {} sender {}", stream_->name(), sender_id_);
    run_read(yield);
    shutdown();
}

void gb28181_tcp_sender_session::run_read(boost::asio::yield_context yield)
{
    boost::system::error_code error;
    std::vector<std::uint8_t> buffer(64 * 1024);
    for (;;)
    {
        transport_->read(buffer, yield, error);
        if (error)
        {
            break;
        }
    }
}

void gb28181_tcp_sender_session::shutdown()
{
    const auto self = shared_from_this();
    boost::asio::post(worker_.io(), [self]() { self->safe_shutdown(); });
}

void gb28181_tcp_sender_session::send_packet(std::vector<std::uint8_t> packet)
{
    if (!transport_)
    {
        return;
    }
    if (packet.size() > std::numeric_limits<std::uint16_t>::max())
    {
        shutdown();
        return;
    }

    const auto length = static_cast<std::uint16_t>(packet.size());
    std::vector<std::uint8_t> frame(packet.size() + 2U);
    frame[0] = static_cast<std::uint8_t>(length >> 8U);
    frame[1] = static_cast<std::uint8_t>(length & 0xffU);
    std::copy(packet.begin(), packet.end(), frame.begin() + 2);
    transport_->write(std::move(frame));
}

void gb28181_tcp_sender_session::safe_shutdown()
{
    if (stream_)
    {
        session_registry::instance().remove_sender_session(stream_->name(), sender_id_, *this);
    }
    if (listener_)
    {
        listener_->shutdown();
    }
    boost::system::error_code error;
    socket_.cancel(error);
    socket_.close(error);
    if (sender_)
    {
        sender_->shutdown();
        sender_.reset();
    }
    if (transport_)
    {
        transport_->shutdown();
    }
    if (stream_)
    {
        spdlog::debug("gb28181 tcp sender shutdown {} sender {}", stream_->name(), sender_id_);
    }
    stream_.reset();
}

}    // namespace media_server
