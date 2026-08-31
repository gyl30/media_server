#include <array>
#include <atomic>
#include <chrono>
#include <limits>
#include <vector>
#include <utility>

#include <spdlog/spdlog.h>
#include <boost/asio/post.hpp>

#include "media/core/stream_registry.h"
#include "media/gb28181/gb28181_udp_session.h"

namespace media_server
{

gb28181_udp_session::gb28181_udp_session(boost::asio::any_io_executor executor,
                                         std::string stream_name,
                                         gb28181_description description)
    : executor_(executor),
      description_(std::move(description)),
      media_(executor, std::move(stream_name), description_.payload_type, description_.ssrc),
      rtcp_timer_(std::move(executor))
{
}

std::optional<gb28181_udp_session::udp_socket_pair> gb28181_udp_session::prepare_udp_sockets(boost::asio::ip::address bind_address)
{
    const auto reserved = port_manager::instance().acquire_pair();
    if (!reserved)
    {
        return std::nullopt;
    }

    const auto local_ports = *reserved;
    const auto weak = weak_from_this();
    boost::system::error_code network_error;
    auto rtp_socket = std::make_shared<udp_socket>(executor_);
    rtp_socket->startup(
        bind_address,
        local_ports.first,
        [weak](boost::system::error_code error, std::span<const std::uint8_t> data, const boost::asio::ip::udp::endpoint& endpoint)
        {
            if (const auto self = weak.lock())
            {
                if (error)
                {
                    self->shutdown();
                    return;
                }
                self->on_rtp(data, endpoint);
            }
        },
        [weak](boost::system::error_code error, const boost::asio::ip::udp::endpoint&)
        {
            if (error)
            {
                if (const auto self = weak.lock())
                {
                    self->shutdown();
                }
            }
        },
        network_error);
    if (network_error)
    {
        port_manager::instance().release(local_ports);
        return std::nullopt;
    }

    auto rtcp_socket = std::make_shared<udp_socket>(executor_);
    rtcp_socket->startup(
        bind_address,
        local_ports.second,
        [weak](boost::system::error_code error, std::span<const std::uint8_t> data, const boost::asio::ip::udp::endpoint& endpoint)
        {
            if (const auto self = weak.lock())
            {
                if (error)
                {
                    self->shutdown();
                    return;
                }
                self->on_rtcp(data, endpoint);
            }
        },
        [weak](boost::system::error_code error, const boost::asio::ip::udp::endpoint&)
        {
            if (error)
            {
                if (const auto self = weak.lock())
                {
                    self->shutdown();
                }
            }
        },
        network_error);
    if (network_error)
    {
        rtp_socket->shutdown([local_ports]() { port_manager::instance().release(local_ports); });
        return std::nullopt;
    }

    return udp_socket_pair{.rtp = std::move(rtp_socket), .rtcp = std::move(rtcp_socket), .local_ports = local_ports};
}

void gb28181_udp_session::shutdown_udp_sockets()
{
    if (local_ports_)
    {
        const auto local_ports = *local_ports_;
        auto remaining = std::make_shared<std::atomic_uint8_t>(2);
        const auto release = [local_ports, remaining]
        {
            if (remaining->fetch_sub(1U, std::memory_order_acq_rel) == 1U)
            {
                port_manager::instance().release(local_ports);
            }
        };
        rtp_socket_->shutdown(release);
        rtcp_socket_->shutdown(release);
        rtp_socket_.reset();
        rtcp_socket_.reset();
        local_ports_.reset();
        return;
    }

    if (rtp_socket_)
    {
        rtp_socket_->shutdown();
        rtp_socket_.reset();
    }
    if (rtcp_socket_)
    {
        rtcp_socket_->shutdown();
        rtcp_socket_.reset();
    }
}

bool gb28181_udp_session::startup()
{
    if (closed_ || description_.transport != gb28181_transport::udp || description_.rtp_port != 0 || description_.rtcp_port != 0 || rtp_socket_ ||
        rtcp_socket_ || description_.address.is_unspecified() || !media_.startup())
    {
        return false;
    }

    auto sockets = prepare_udp_sockets(description_.address);
    if (!sockets)
    {
        media_.shutdown();
        return false;
    }
    rtp_socket_ = std::move(sockets->rtp);
    rtcp_socket_ = std::move(sockets->rtcp);
    local_ports_ = sockets->local_ports;
    schedule_rtcp();
    spdlog::info("gb28181 udp session started stream {} rtp_port {} rtcp_port {}",
                 media_.stream_name(),
                 local_ports_->first,
                 local_ports_->second);
    return true;
}

void gb28181_udp_session::shutdown()
{
    const auto self = shared_from_this();
    boost::asio::post(executor_, [self]() { self->safe_shutdown(); });
}

const std::string& gb28181_udp_session::stream_name() const noexcept { return media_.stream_name(); }

std::optional<port_manager_impl::port_pair> gb28181_udp_session::local_ports() const noexcept { return local_ports_; }

void gb28181_udp_session::on_rtp(std::span<const std::uint8_t> data, const boost::asio::ip::udp::endpoint& endpoint)
{
    if (closed_)
    {
        return;
    }

    const auto result = media_.input_rtp(data);
    if (result == gb28181_rtp_input_result::fatal)
    {
        shutdown();
        return;
    }
    if (result == gb28181_rtp_input_result::accepted && !remote_rtp_endpoint_)
    {
        if (!rtp_socket_)
        {
            shutdown();
            return;
        }
        boost::system::error_code network_error;
        rtp_socket_->connect(endpoint, network_error);
        if (network_error)
        {
            shutdown();
            return;
        }
        remote_rtp_endpoint_ = endpoint;
    }
}

void gb28181_udp_session::on_rtcp(std::span<const std::uint8_t> data, const boost::asio::ip::udp::endpoint& endpoint)
{
    if (closed_ || media_.input_rtcp(data) <= 0 || remote_rtcp_endpoint_ || !remote_rtp_endpoint_ ||
        endpoint.address() != remote_rtp_endpoint_->address())
    {
        return;
    }
    boost::system::error_code network_error;
    rtcp_socket_->connect(endpoint, network_error);
    if (network_error)
    {
        shutdown();
        return;
    }
    remote_rtcp_endpoint_ = endpoint;
}

void gb28181_udp_session::schedule_rtcp()
{
    rtcp_timer_.expires_after(std::chrono::seconds(1));
    const auto weak = weak_from_this();
    rtcp_timer_.async_wait(
        [weak](const boost::system::error_code& error)
        {
            const auto self = weak.lock();
            if (!self || error || self->closed_)
            {
                return;
            }

            std::optional<boost::asio::ip::udp::endpoint> target = self->remote_rtcp_endpoint_;
            if (!target && self->remote_rtp_endpoint_ && self->remote_rtp_endpoint_->port() != std::numeric_limits<std::uint16_t>::max())
            {
                target.emplace(self->remote_rtp_endpoint_->address(), static_cast<std::uint16_t>(self->remote_rtp_endpoint_->port() + 1U));
            }
            if (target && self->rtcp_socket_)
            {
                std::array<std::uint8_t, 1500> buffer{};
                const auto bytes = self->media_.generate_rtcp(buffer);
                if (bytes > 0)
                {
                    self->rtcp_socket_->send(std::vector<std::uint8_t>(buffer.begin(), buffer.begin() + bytes), *target);
                }
            }
            self->schedule_rtcp();
        });
}

void gb28181_udp_session::safe_shutdown()
{
    if (closed_)
    {
        return;
    }
    closed_ = true;
    registry::instance().remove_input_session(stream_name(), *this);
    rtcp_timer_.cancel();
    media_.shutdown();
    shutdown_udp_sockets();
    remote_rtp_endpoint_.reset();
    remote_rtcp_endpoint_.reset();
    spdlog::debug("gb28181 udp session shutdown {}", media_.stream_name());
}

}    // namespace media_server
