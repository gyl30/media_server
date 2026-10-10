#include <array>
#include <vector>
#include <utility>
#include <algorithm>

#include <spdlog/spdlog.h>
#include <boost/asio/post.hpp>
#include <boost/asio/error.hpp>

#include "media/net/worker_context.h"
#include "media/gb28181/gb28181_rtp_sender.h"
#include "media/gb28181/gb28181_tcp_sender_session.h"

namespace media_server
{
gb28181_tcp_sender_session::gb28181_tcp_sender_session(worker_context& worker, std::shared_ptr<media_stream> stream, std::string sender_id)
    : worker_(worker), stream_(std::move(stream)), sender_id_(std::move(sender_id)), socket_(worker_.io())
{
}

void gb28181_tcp_sender_session::startup(boost::asio::ip::tcp::endpoint remote_endpoint, std::uint8_t payload_type, std::uint32_t ssrc)
{
    const auto self = shared_from_this();
    worker_.spawn(
        [self, remote_endpoint = std::move(remote_endpoint), payload_type, ssrc](boost::asio::yield_context yield) mutable
        { self->run(std::move(remote_endpoint), payload_type, ssrc, yield); });
}

bool gb28181_tcp_sender_session::startup(boost::asio::ip::address bind_address,
                                         std::uint16_t listen_port,
                                         std::uint8_t payload_type,
                                         std::uint32_t ssrc)
{
    listener_ = std::make_unique<tcp_listener>(worker_.io(), listen_port, std::move(bind_address));
    boost::system::error_code error;
    listener_->startup(error);
    if (error)
    {
        spdlog::error("gb28181 tcp sender listener startup failed stream {} sender {} error {}", stream_->stream_id(), sender_id_, error.message());
        return false;
    }

    const auto self = shared_from_this();
    worker_.spawn([self, payload_type, ssrc](boost::asio::yield_context yield) { self->run(std::nullopt, payload_type, ssrc, yield); });
    return true;
}

void gb28181_tcp_sender_session::run(std::optional<boost::asio::ip::tcp::endpoint> remote_endpoint,
                                     std::uint8_t payload_type,
                                     std::uint32_t ssrc,
                                     boost::asio::yield_context yield)
{
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
            spdlog::warn("gb28181 tcp sender establishment failed stream {} sender {} error {}", stream_->stream_id(), sender_id_, error.message());
        }
        shutdown();
        return;
    }

    transport_ = std::make_shared<tcp_transport>(std::move(socket_));
    const auto self = shared_from_this();
    transport_->set_write_callback(
        [weak = std::weak_ptr<gb28181_tcp_sender_session>(self)](boost::system::error_code write_error, std::size_t)
        {
            if (write_error)
            {
                if (const auto owner = weak.lock())
                {
                    owner->shutdown();
                }
            }
        });
    sender_ = std::make_shared<gb28181_rtp_sender>(
        worker_, stream_, [self](std::vector<std::uint8_t> packet) { self->send_packet(std::move(packet)); }, [self]() { self->shutdown(); });
    if (!sender_->startup(payload_type, ssrc))
    {
        shutdown();
        return;
    }

    spdlog::info("gb28181 tcp sender started stream {} sender {}", stream_->stream_id(), sender_id_);
    run_read(yield);
    shutdown();
}

void gb28181_tcp_sender_session::run_read(boost::asio::yield_context yield)
{
    std::array<std::uint8_t, 1> buffer{};
    boost::system::error_code error;
    for (;;)
    {
        transport_->read(buffer, yield, error);
        if (error)
        {
            return;
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
    const auto length = static_cast<std::uint16_t>(packet.size());
    std::vector<std::uint8_t> frame(packet.size() + 2U);
    frame[0] = static_cast<std::uint8_t>(length >> 8U);
    frame[1] = static_cast<std::uint8_t>(length & 0xffU);
    std::copy(packet.begin(), packet.end(), frame.begin() + 2);
    transport_->write(std::move(frame));
}

void gb28181_tcp_sender_session::safe_shutdown()
{
    if (!stream_)
    {
        return;
    }
    auto stream = std::move(stream_);
    session_registry::instance().remove_sender_session(stream->stream_id(), sender_id_, *this);
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
    if (sender_)
    {
        sender_->shutdown();
        sender_.reset();
    }
    spdlog::debug("gb28181 tcp sender shutdown {} sender {}", stream->stream_id(), sender_id_);
}

}    // namespace media_server
