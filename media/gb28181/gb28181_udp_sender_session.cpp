#include <array>
#include <chrono>
#include <utility>

#include <spdlog/spdlog.h>
#include <boost/asio/post.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/spawn.hpp>

#include "media/net/worker_context.h"
#include "media/gb28181/gb28181_rtp_sender.h"
#include "media/gb28181/gb28181_udp_sender_session.h"

extern "C"
{
#include "rtp.h"
}

namespace media_server
{
namespace
{
constexpr auto rtcp_interval = std::chrono::seconds{25};
constexpr std::size_t max_write_queue_bytes = 1024U * 1024U;
}    // namespace

gb28181_udp_sender_session::gb28181_udp_sender_session(worker_context& worker,
                                                       std::shared_ptr<media_stream> stream,
                                                       std::string sender_id,
                                                       boost::asio::ip::udp::endpoint remote_rtp_endpoint,
                                                       std::optional<boost::asio::ip::udp::endpoint> remote_rtcp_endpoint)
    : worker_(worker),
      stream_(std::move(stream)),
      sender_id_(std::move(sender_id)),
      remote_rtp_endpoint_(std::move(remote_rtp_endpoint)),
      remote_rtcp_endpoint_(std::move(remote_rtcp_endpoint)),
      rtp_transport_(std::make_shared<udp_transport>(worker_.io())),
      rtcp_transport_(std::make_shared<udp_transport>(worker_.io())),
      rtcp_timer_(worker_.io())
{
}

void gb28181_udp_sender_session::shutdown_udp_transports()
{
    rtp_transport_->shutdown();
    rtcp_transport_->shutdown();
    if (local_ports_)
    {
        media_port_pool::instance().release(*local_ports_);
        local_ports_.reset();
    }
}

bool gb28181_udp_sender_session::startup(boost::asio::ip::address bind_address, std::uint8_t payload_type, std::uint32_t ssrc)
{
    boost::system::error_code network_error;
    auto local_ports = media_port_pool::instance().acquire_pair_and_bind(*rtp_transport_, *rtcp_transport_, bind_address, network_error);
    if (!local_ports)
    {
        return false;
    }
    local_ports_ = *local_ports;
    shutdown_subscription_ = worker_.subscribe_shutdown([self = shared_from_this()]() { self->safe_shutdown(); });
    if (!shutdown_subscription_)
    {
        shutdown_udp_transports();
        return false;
    }

    if (remote_rtcp_endpoint_)
    {
        rtp_event_t handler{};
        rtcp_sender_ = rtp_create(&handler, nullptr, ssrc, 0, 90'000, 2 * 1024 * 1024, 1);
        if (rtcp_sender_ == nullptr)
        {
            shutdown_subscription_.reset();
            shutdown_udp_transports();
            return false;
        }
    }

    const auto self = shared_from_this();
    rtp_transport_->set_write_callback(
        [weak = weak_from_this()](boost::system::error_code error, std::size_t)
        {
            const auto locked = weak.lock();
            if (!locked || !locked->stream_ || error == boost::asio::error::operation_aborted)
            {
                return;
            }
            if (error == boost::asio::error::no_buffer_space)
            {
                spdlog::warn("gb28181 udp write queue full stream {} sender {}", locked->stream_->name(), locked->sender_id_);
                return;
            }
            if (error)
            {
                locked->sender_->shutdown();
                locked->shutdown();
                return;
            }
            locked->queued_write_bytes_ -= locked->write_queue_.front().size();
            locked->write_queue_.pop_front();
            locked->start_rtp_write();
        });
    rtcp_transport_->set_write_callback(
        [weak = weak_from_this()](boost::system::error_code error, std::size_t)
        {
            const auto locked = weak.lock();
            if (!locked || !locked->stream_ || error == boost::asio::error::operation_aborted)
            {
                return;
            }
            if (error && error != boost::asio::error::no_buffer_space)
            {
                locked->sender_->shutdown();
                locked->shutdown();
                return;
            }
            locked->schedule_rtcp();
        });
    sender_ = std::make_shared<gb28181_rtp_sender>(
        worker_, stream_, [self](std::vector<std::uint8_t> packet) { self->send_packet(std::move(packet)); }, [self]() { self->shutdown(); });
    if (!sender_->startup(payload_type, ssrc))
    {
        sender_->shutdown();
        sender_.reset();
        shutdown_subscription_.reset();
        shutdown_udp_transports();
        if (rtcp_sender_ != nullptr)
        {
            rtp_destroy(rtcp_sender_);
            rtcp_sender_ = nullptr;
        }
        return false;
    }

    spdlog::info("gb28181 udp sender started stream {} local_rtp_port {} local_rtcp_port {} remote_rtp {}:{} remote_rtcp {}:{} rtcp {}",
                 stream_->name(),
                 local_ports_->first,
                 local_ports_->second,
                 remote_rtp_endpoint_.address().to_string(),
                 remote_rtp_endpoint_.port(),
                 remote_rtcp_endpoint_ ? remote_rtcp_endpoint_->address().to_string() : std::string{},
                 remote_rtcp_endpoint_ ? remote_rtcp_endpoint_->port() : 0,
                 remote_rtcp_endpoint_.has_value());
    return true;
}

void gb28181_udp_sender_session::shutdown()
{
    const auto self = shared_from_this();
    boost::asio::post(worker_.io(), [self]() { self->safe_shutdown(); });
}

void gb28181_udp_sender_session::start_rtp_write()
{
    if (stream_ && !write_queue_.empty())
    {
        rtp_transport_->write(std::span<const std::uint8_t>{write_queue_.front()}, remote_rtp_endpoint_);
    }
}

void gb28181_udp_sender_session::schedule_rtcp()
{
    if (!stream_)
    {
        return;
    }

    rtcp_timer_.expires_after(rtcp_interval);
    const auto self = shared_from_this();
    rtcp_timer_.async_wait(
        [self](const boost::system::error_code& error)
        {
            if (error || !self->stream_)
            {
                return;
            }

            std::array<std::uint8_t, 1500> buffer{};
            const auto bytes = rtp_rtcp_report(self->rtcp_sender_, buffer.data(), static_cast<int>(buffer.size()));
            if (bytes <= 0 || bytes > static_cast<int>(buffer.size()))
            {
                self->schedule_rtcp();
                return;
            }

            std::vector<std::uint8_t> packet(buffer.begin(), buffer.begin() + bytes);
            self->rtcp_transport_->write(std::move(packet), *self->remote_rtcp_endpoint_);
        });
}

void gb28181_udp_sender_session::send_packet(std::vector<std::uint8_t> packet)
{
    if (packet.size() > max_write_queue_bytes || queued_write_bytes_ > max_write_queue_bytes - packet.size())
    {
        spdlog::warn("gb28181 udp write queue full stream {} sender {} queued {} limit {} dropped {}",
                     stream_->name(),
                     sender_id_,
                     queued_write_bytes_,
                     max_write_queue_bytes,
                     packet.size());
        return;
    }
    if (remote_rtcp_endpoint_ && rtp_onsend(rtcp_sender_, packet.data(), static_cast<int>(packet.size())) != 0)
    {
        sender_->shutdown();
        shutdown();
        return;
    }

    const bool start_write = write_queue_.empty();
    queued_write_bytes_ += packet.size();
    write_queue_.push_back(std::move(packet));
    if (start_write)
    {
        start_rtp_write();
    }

    if (remote_rtcp_endpoint_ && !rtcp_reporting_started_)
    {
        rtcp_reporting_started_ = true;
        schedule_rtcp();
    }
}

void gb28181_udp_sender_session::safe_shutdown()
{
    if (!stream_)
    {
        return;
    }
    auto stream = std::move(stream_);
    shutdown_subscription_.reset();
    session_registry::instance().remove_sender_session(stream->name(), sender_id_, *this);
    rtcp_timer_.cancel();
    if (sender_)
    {
        sender_->shutdown();
        sender_.reset();
    }
    shutdown_udp_transports();
    if (rtcp_sender_ != nullptr)
    {
        rtp_destroy(rtcp_sender_);
        rtcp_sender_ = nullptr;
    }
    write_queue_.clear();
    queued_write_bytes_ = 0;
    spdlog::debug("gb28181 udp sender shutdown {} sender {}", stream->name(), sender_id_);
}

}    // namespace media_server
