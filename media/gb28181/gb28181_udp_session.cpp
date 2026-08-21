#include "media/gb28181/gb28181_udp_session.h"

#include <boost/asio/post.hpp>

#include <spdlog/spdlog.h>

#include <array>
#include <chrono>
#include <utility>
#include <vector>

namespace media_server
{

gb28181_udp_session::gb28181_udp_session(stream_registry& registry,
                                         boost::asio::any_io_executor executor,
                                         std::string stream_name,
                                         gb28181_description description)
    : executor_(executor),
      description_(std::move(description)),
      media_(registry, executor, std::move(stream_name), description_.payload_type, description_.ssrc),
      rtcp_timer_(std::move(executor))
{
}

bool gb28181_udp_session::startup()
{
    if (closed_ || rtp_socket_ || rtcp_socket_ || !media_.startup())
    {
        return false;
    }

    const auto self = shared_from_this();
    const auto bind_address = description_.address.is_v4() ? boost::asio::ip::address{boost::asio::ip::address_v4::any()}
                                                           : boost::asio::ip::address{boost::asio::ip::address_v6::any()};
    auto rtp_socket = std::make_shared<udp_socket>(executor_);
    if (!rtp_socket->startup(
            bind_address,
            description_.rtp_port,
            [self](boost::system::error_code error, std::span<const std::uint8_t> data, const boost::asio::ip::udp::endpoint& endpoint)
            {
                if (error)
                {
                    self->shutdown();
                    return;
                }
                self->on_rtp(data, endpoint);
            },
            [self](boost::system::error_code error, const boost::asio::ip::udp::endpoint&)
            {
                if (error)
                {
                    self->shutdown();
                }
            }))
    {
        media_.shutdown();
        return false;
    }

    auto rtcp_socket = std::make_shared<udp_socket>(executor_);
    if (!rtcp_socket->startup(
            bind_address,
            description_.rtcp_port,
            [self](boost::system::error_code error, std::span<const std::uint8_t> data, const boost::asio::ip::udp::endpoint& endpoint)
            {
                if (error)
                {
                    self->shutdown();
                    return;
                }
                self->on_rtcp(data, endpoint);
            },
            [self](boost::system::error_code error, const boost::asio::ip::udp::endpoint&)
            {
                if (error)
                {
                    self->shutdown();
                }
            }))
    {
        rtp_socket->shutdown();
        media_.shutdown();
        return false;
    }

    rtp_socket_ = std::move(rtp_socket);
    rtcp_socket_ = std::move(rtcp_socket);
    wait_rtcp();
    spdlog::info("gb28181 udp session started stream {} rtp_port {} rtcp_port {}", media_.stream_name(), description_.rtp_port, description_.rtcp_port);
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
    static_cast<void>(endpoint);
    if (closed_)
    {
        return;
    }
    if (!media_.input_rtp(data))
    {
        shutdown();
    }
}

void gb28181_udp_session::on_rtcp(std::span<const std::uint8_t> data, const boost::asio::ip::udp::endpoint& endpoint)
{
    if (closed_)
    {
        return;
    }
    if (media_.input_rtcp(data) > 0)
    {
        remote_rtcp_endpoint_ = endpoint;
    }
}

void gb28181_udp_session::wait_rtcp()
{
    rtcp_timer_.expires_after(std::chrono::seconds(1));
    const auto self = shared_from_this();
    rtcp_timer_.async_wait(
        [self](const boost::system::error_code& error)
        {
            if (error || self->closed_)
            {
                return;
            }

            if (self->remote_rtcp_endpoint_ && self->rtcp_socket_)
            {
                std::array<std::uint8_t, 1500> buffer{};
                const auto bytes = self->media_.rtcp(buffer);
                if (bytes > 0)
                {
                    self->rtcp_socket_->send(std::vector<std::uint8_t>(buffer.begin(), buffer.begin() + bytes), *self->remote_rtcp_endpoint_);
                }
            }
            self->wait_rtcp();
        });
}

void gb28181_udp_session::safe_shutdown()
{
    if (closed_)
    {
        return;
    }
    closed_ = true;
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
    remote_rtcp_endpoint_.reset();
    spdlog::debug("gb28181 udp session shutdown {}", media_.stream_name());
}

}    // namespace media_server
