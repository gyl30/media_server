#include <array>
#include <chrono>
#include <utility>

#include <spdlog/spdlog.h>
#include <boost/asio/post.hpp>

#include "media/net/udp_socket.h"
#include "media/gb28181/gb28181_output_media.h"
#include "media/core/stream_registry.h"
#include "media/gb28181/gb28181_udp_output_session.h"

extern "C"
{
#include "rtp.h"
}

namespace media_server
{
namespace
{
constexpr int udp_port_pair_attempts = 32;
}

gb28181_udp_output_session::gb28181_udp_output_session(boost::asio::any_io_executor executor,
                                                       std::shared_ptr<media_stream> stream,
                                                       gb28181_description description,
                                                       std::string output_id,
                                                       bool rtcp_enabled)
    : executor_(executor),
      stream_(std::move(stream)),
      stream_name_(stream_ ? stream_->name() : std::string{}),
      output_id_(std::move(output_id)),
      description_(std::move(description)),
      remote_rtp_endpoint_(description_.address, description_.rtp_port),
      remote_rtcp_endpoint_(description_.address, description_.rtcp_port),
      rtcp_timer_(std::move(executor)),
      rtcp_enabled_(rtcp_enabled)
{
}

std::optional<gb28181_udp_output_session::udp_socket_pair> gb28181_udp_output_session::prepare_udp_sockets(
    boost::asio::ip::address bind_address)
{
    const auto weak = weak_from_this();
    const auto start_socket = [weak, executor = executor_, &bind_address](std::uint16_t port)
    {
        auto socket = std::make_shared<udp_socket>(executor);
        if (!socket->startup(
                bind_address,
                port,
                [weak](boost::system::error_code error, std::span<const std::uint8_t>, const boost::asio::ip::udp::endpoint&)
                {
                    if (error)
                    {
                        if (const auto self = weak.lock())
                        {
                            self->shutdown();
                        }
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
                }))
        {
            return std::shared_ptr<udp_socket>{};
        }
        return socket;
    };

    for (int attempt = 0; attempt < udp_port_pair_attempts; ++attempt)
    {
        auto first = start_socket(0);
        if (!first)
        {
            break;
        }
        const auto first_port = first->local_port();
        const auto second_port = static_cast<std::uint16_t>((first_port & 1U) == 0 ? first_port + 1U : first_port - 1U);
        auto second = second_port == 0 ? std::shared_ptr<udp_socket>{} : start_socket(second_port);
        if (!second)
        {
            first->shutdown();
            continue;
        }

        if ((first_port & 1U) == 0)
        {
            return udp_socket_pair{.rtp = std::move(first), .rtcp = std::move(second)};
        }
        return udp_socket_pair{.rtp = std::move(second), .rtcp = std::move(first)};
    }
    return std::nullopt;
}

bool gb28181_udp_output_session::startup()
{
    if (closed_ || rtp_socket_ || rtcp_socket_ || media_ || !stream_ || description_.transport != gb28181_transport::udp ||
        description_.address.is_unspecified())
    {
        return false;
    }

    const auto bind_address = description_.address.is_v4() ? boost::asio::ip::address{boost::asio::ip::address_v4::any()}
                                                           : boost::asio::ip::address{boost::asio::ip::address_v6::any()};
    auto sockets = prepare_udp_sockets(bind_address);
    if (!sockets)
    {
        return false;
    }
    rtp_socket_ = std::move(sockets->rtp);
    rtcp_socket_ = std::move(sockets->rtcp);

    if (rtcp_enabled_)
    {
        rtp_event_t handler{};
        rtcp_sender_ = rtp_create(&handler, nullptr, description_.ssrc, 0, 90'000, 2 * 1024 * 1024, 1);
        if (rtcp_sender_ == nullptr)
        {
            rtp_socket_->shutdown();
            rtcp_socket_->shutdown();
            rtp_socket_.reset();
            rtcp_socket_.reset();
            return false;
        }
    }

    const auto weak = weak_from_this();
    media_ = std::make_shared<gb28181_output_media>(
        executor_,
        stream_,
        description_.payload_type,
        description_.ssrc,
        [weak](std::vector<std::uint8_t> packet)
        {
            if (const auto session = weak.lock())
            {
                session->send_packet(std::move(packet));
            }
        },
        [weak]()
        {
            if (const auto session = weak.lock())
            {
                session->shutdown();
            }
        });
    if (!media_->startup())
    {
        media_.reset();
        rtp_socket_->shutdown();
        rtcp_socket_->shutdown();
        rtp_socket_.reset();
        rtcp_socket_.reset();
        if (rtcp_sender_ != nullptr)
        {
            rtp_destroy(rtcp_sender_);
            rtcp_sender_ = nullptr;
        }
        return false;
    }

    spdlog::info("gb28181 udp output started stream {} local_rtp_port {} local_rtcp_port {} remote_rtp {}:{} remote_rtcp {}:{} rtcp {}",
                 stream_name_,
                 rtp_socket_->local_port(),
                 rtcp_socket_->local_port(),
                 description_.address.to_string(),
                 description_.rtp_port,
                 description_.address.to_string(),
                 description_.rtcp_port,
                 rtcp_enabled_);
    return true;
}

void gb28181_udp_output_session::shutdown()
{
    const auto self = shared_from_this();
    boost::asio::post(executor_, [self]() { self->safe_shutdown(); });
}

void gb28181_udp_output_session::send_packet(std::vector<std::uint8_t> packet)
{
    if (closed_ || !rtp_socket_)
    {
        return;
    }
    if (rtcp_sender_ != nullptr && rtp_onsend(rtcp_sender_, packet.data(), static_cast<int>(packet.size())) != 0)
    {
        shutdown();
        return;
    }

    rtp_socket_->send(std::move(packet), remote_rtp_endpoint_);
    if (rtcp_sender_ != nullptr && !rtcp_started_)
    {
        rtcp_started_ = true;
        wait_rtcp();
    }
}

void gb28181_udp_output_session::wait_rtcp()
{
    rtcp_timer_.expires_after(std::chrono::seconds(25));
    const auto weak = weak_from_this();
    rtcp_timer_.async_wait(
        [weak](const boost::system::error_code& error)
        {
            const auto self = weak.lock();
            if (!self || error || self->closed_ || self->rtcp_sender_ == nullptr || !self->rtcp_socket_)
            {
                return;
            }

            std::array<std::uint8_t, 1500> buffer{};
            const auto bytes = rtp_rtcp_report(self->rtcp_sender_, buffer.data(), static_cast<int>(buffer.size()));
            if (bytes > 0 && bytes <= static_cast<int>(buffer.size()))
            {
                self->rtcp_socket_->send(std::vector<std::uint8_t>(buffer.begin(), buffer.begin() + bytes), self->remote_rtcp_endpoint_);
            }
            self->wait_rtcp();
        });
}

void gb28181_udp_output_session::safe_shutdown()
{
    if (closed_)
    {
        return;
    }
    closed_ = true;
    registry::instance().remove_output_session(stream_name_, output_id_, *this);
    rtcp_timer_.cancel();
    if (media_)
    {
        media_->shutdown();
        media_.reset();
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
    if (rtcp_sender_ != nullptr)
    {
        rtp_destroy(rtcp_sender_);
        rtcp_sender_ = nullptr;
    }
    rtcp_started_ = false;
    stream_.reset();
    spdlog::debug("gb28181 udp output shutdown {} output {}", stream_name_, output_id_);
}

}    // namespace media_server
