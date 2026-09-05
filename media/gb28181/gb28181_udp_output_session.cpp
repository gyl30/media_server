#include <array>
#include <chrono>
#include <span>
#include <utility>

#include <spdlog/spdlog.h>
#include <boost/asio/detached.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/spawn.hpp>

#include "media/net/worker_context.h"
#include "media/core/stream_registry.h"
#include "media/gb28181/gb28181_output_media.h"
#include "media/gb28181/gb28181_udp_output_session.h"

extern "C"
{
#include "rtp.h"
}

namespace media_server
{
gb28181_udp_output_session::gb28181_udp_output_session(worker_context& worker,
                                                       std::shared_ptr<media_stream> stream,
                                                       gb28181_description description,
                                                       boost::asio::ip::address bind_address,
                                                       std::string output_id,
                                                       bool rtcp_enabled)
    : worker_(worker),
      stream_(std::move(stream)),
      stream_name_(stream_ ? stream_->name() : std::string{}),
      output_id_(std::move(output_id)),
      description_(std::move(description)),
      bind_address_(std::move(bind_address)),
      remote_rtp_endpoint_(description_.address, description_.rtp_port),
      remote_rtcp_endpoint_(description_.address, description_.rtcp_port),
      rtp_transport_(worker_.io()),
      rtcp_transport_(worker_.io()),
      rtcp_timer_(worker_.io()),
      rtcp_enabled_(rtcp_enabled)
{
}

std::optional<port_manager_impl::port_pair> gb28181_udp_output_session::prepare_udp_transports(boost::asio::ip::address bind_address)
{
    const auto reserved = port_manager::instance().acquire_pair();
    if (!reserved)
    {
        return std::nullopt;
    }

    const auto local_ports = *reserved;
    boost::system::error_code network_error;
    rtp_transport_.startup(bind_address, local_ports.first, network_error);
    if (network_error)
    {
        port_manager::instance().release(local_ports);
        return std::nullopt;
    }

    rtcp_transport_.startup(std::move(bind_address), local_ports.second, network_error);
    if (network_error)
    {
        rtp_transport_.shutdown();
        port_manager::instance().release(local_ports);
        return std::nullopt;
    }

    return local_ports;
}

void gb28181_udp_output_session::shutdown_udp_transports()
{
    rtp_transport_.shutdown();
    rtcp_transport_.shutdown();
    if (local_ports_)
    {
        port_manager::instance().release(*local_ports_);
        local_ports_.reset();
    }
}

bool gb28181_udp_output_session::startup()
{
    if (local_ports_ || media_ || !stream_ || description_.transport != gb28181_transport::udp || description_.address.is_unspecified() ||
        bind_address_.is_unspecified())
    {
        return false;
    }

    auto local_ports = prepare_udp_transports(bind_address_);
    if (!local_ports)
    {
        return false;
    }
    local_ports_ = *local_ports;

    if (rtcp_enabled_)
    {
        rtp_event_t handler{};
        rtcp_sender_ = rtp_create(&handler, nullptr, description_.ssrc, 0, 90'000, 2 * 1024 * 1024, 1);
        if (rtcp_sender_ == nullptr)
        {
            shutdown_udp_transports();
            return false;
        }
    }

    const auto weak = weak_from_this();
    media_ = std::make_shared<gb28181_output_media>(
        worker_,
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
        media_->shutdown();
        media_.reset();
        shutdown_udp_transports();
        if (rtcp_sender_ != nullptr)
        {
            rtp_destroy(rtcp_sender_);
            rtcp_sender_ = nullptr;
        }
        return false;
    }

    spdlog::info("gb28181 udp output started stream {} local_rtp_port {} local_rtcp_port {} remote_rtp {}:{} remote_rtcp {}:{} rtcp {}",
                 stream_name_,
                 local_ports_->first,
                 local_ports_->second,
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
    boost::asio::post(worker_.io(), [self]() { self->safe_shutdown(); });
}

void gb28181_udp_output_session::run_rtp_write(boost::asio::yield_context yield)
{
    for (;;)
    {
        if (write_queue_.empty())
        {
            return;
        }

        const auto data = write_queue_.front();
        boost::system::error_code error;
        static_cast<void>(
            rtp_transport_.write(std::span<const std::uint8_t>{data->data(), data->size()}, remote_rtp_endpoint_, yield, error));
        if (error)
        {
            shutdown();
            return;
        }

        write_queue_.pop_front();
    }
}

void gb28181_udp_output_session::run_rtcp_sender(boost::asio::yield_context yield)
{
    boost::system::error_code error;
    for (;;)
    {
        rtcp_timer_.expires_after(std::chrono::seconds(25));
        rtcp_timer_.async_wait(yield[error]);
        if (error)
        {
            break;
        }

        std::array<std::uint8_t, 1500> buffer{};
        const auto bytes = rtp_rtcp_report(rtcp_sender_, buffer.data(), static_cast<int>(buffer.size()));
        if (bytes <= 0 || bytes > static_cast<int>(buffer.size()))
        {
            continue;
        }

        static_cast<void>(
            rtcp_transport_.write(std::span{buffer.data(), static_cast<std::size_t>(bytes)}, remote_rtcp_endpoint_, yield, error));
        if (error)
        {
            break;
        }
    }

    shutdown();
}

void gb28181_udp_output_session::send_packet(std::vector<std::uint8_t> packet)
{
    if (!local_ports_)
    {
        return;
    }
    if (rtcp_sender_ != nullptr && rtp_onsend(rtcp_sender_, packet.data(), static_cast<int>(packet.size())) != 0)
    {
        shutdown();
        return;
    }

    const bool start_write = write_queue_.empty();
    write_queue_.push_back(std::make_shared<std::vector<std::uint8_t>>(std::move(packet)));
    if (start_write)
    {
        const auto self = shared_from_this();
        boost::asio::spawn(worker_.io(), [self](boost::asio::yield_context yield) { self->run_rtp_write(yield); }, boost::asio::detached);
    }

    if (rtcp_sender_ != nullptr && !rtcp_started_)
    {
        rtcp_started_ = true;
        const auto self = shared_from_this();
        boost::asio::spawn(worker_.io(), [self](boost::asio::yield_context yield) { self->run_rtcp_sender(yield); }, boost::asio::detached);
    }
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
    shutdown_udp_transports();
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
