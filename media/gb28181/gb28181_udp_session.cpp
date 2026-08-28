#include <array>
#include <chrono>
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
                                         gb28181_description description,
                                         gb28181_udp_peer peer)
    : executor_(executor),
      description_(std::move(description)),
      peer_(std::move(peer)),
      media_(executor, std::move(stream_name), description_.payload_type, description_.ssrc),
      rtcp_timer_(std::move(executor))
{
}

bool gb28181_udp_session::startup()
{
    if (closed_ || description_.transport != gb28181_transport::udp || rtp_socket_ || rtcp_socket_ || peer_.rtcp_port == 0 || !media_.startup())
    {
        return false;
    }

    const auto weak = weak_from_this();
    const auto bind_address = description_.address.is_v4() ? boost::asio::ip::address{boost::asio::ip::address_v4::any()}
                                                           : boost::asio::ip::address{boost::asio::ip::address_v6::any()};
    boost::system::error_code network_error;
    auto rtp_socket = std::make_shared<udp_socket>(executor_);
    rtp_socket->startup(
        bind_address,
        description_.rtp_port,
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
        media_.shutdown();
        return false;
    }

    if (peer_.rtp)
    {
        rtp_socket->connect(*peer_.rtp, network_error);
        if (network_error)
        {
            rtp_socket->shutdown();
            media_.shutdown();
            return false;
        }
    }

    auto rtcp_socket = std::make_shared<udp_socket>(executor_);
    rtcp_socket->startup(
        bind_address,
        description_.rtcp_port,
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
        rtp_socket->shutdown();
        media_.shutdown();
        return false;
    }

    if (peer_.rtp)
    {
        const boost::asio::ip::udp::endpoint rtcp_endpoint{peer_.rtp->address(), peer_.rtcp_port};
        rtcp_socket->connect(rtcp_endpoint, network_error);
        if (network_error)
        {
            rtp_socket->shutdown();
            rtcp_socket->shutdown();
            media_.shutdown();
            return false;
        }
        remote_rtp_endpoint_ = *peer_.rtp;
        remote_rtcp_endpoint_ = rtcp_endpoint;
    }

    rtp_socket_ = std::move(rtp_socket);
    rtcp_socket_ = std::move(rtcp_socket);
    schedule_rtcp();
    spdlog::info(
        "gb28181 udp session started stream {} rtp_port {} rtcp_port {}", media_.stream_name(), description_.rtp_port, description_.rtcp_port);
    return true;
}

void gb28181_udp_session::shutdown()
{
    const auto self = shared_from_this();
    boost::asio::post(executor_, [self]() { self->safe_shutdown(); });
}

const std::string& gb28181_udp_session::stream_name() const noexcept { return media_.stream_name(); }

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
        if (!rtp_socket_ || !rtcp_socket_)
        {
            shutdown();
            return;
        }
        const boost::asio::ip::udp::endpoint rtcp_endpoint{endpoint.address(), peer_.rtcp_port};
        boost::system::error_code network_error;
        rtp_socket_->connect(endpoint, network_error);
        if (!network_error)
        {
            rtcp_socket_->connect(rtcp_endpoint, network_error);
        }
        if (network_error)
        {
            shutdown();
            return;
        }
        remote_rtp_endpoint_ = endpoint;
        remote_rtcp_endpoint_ = rtcp_endpoint;
    }
}

void gb28181_udp_session::on_rtcp(std::span<const std::uint8_t> data, const boost::asio::ip::udp::endpoint&)
{
    if (closed_ || !remote_rtcp_endpoint_)
    {
        return;
    }
    static_cast<void>(media_.input_rtcp(data));
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

            if (self->remote_rtcp_endpoint_ && self->rtcp_socket_)
            {
                std::array<std::uint8_t, 1500> buffer{};
                const auto bytes = self->media_.generate_rtcp(buffer);
                if (bytes > 0)
                {
                    self->rtcp_socket_->send(std::vector<std::uint8_t>(buffer.begin(), buffer.begin() + bytes), *self->remote_rtcp_endpoint_);
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
    remote_rtp_endpoint_.reset();
    remote_rtcp_endpoint_.reset();
    spdlog::debug("gb28181 udp session shutdown {}", media_.stream_name());
}

}    // namespace media_server
