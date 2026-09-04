#include <array>
#include <chrono>
#include <limits>
#include <span>
#include <vector>
#include <utility>

#include <spdlog/spdlog.h>
#include <boost/asio/detached.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/spawn.hpp>

#include "media/core/stream_registry.h"
#include "media/net/worker_context.h"
#include "media/gb28181/gb28181_udp_session.h"

namespace media_server
{

gb28181_udp_session::gb28181_udp_session(worker_context& worker,
                                         std::string stream_name,
                                         gb28181_description description)
    : worker_(worker),
      description_(std::move(description)),
      media_(worker_, std::move(stream_name), description_.payload_type, description_.ssrc),
      rtp_transport_(worker_.io()),
      rtcp_transport_(worker_.io()),
      rtcp_timer_(worker_.io())
{
}

std::optional<port_manager_impl::port_pair> gb28181_udp_session::prepare_udp_transports(boost::asio::ip::address bind_address)
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

bool gb28181_udp_session::startup()
{
    if (description_.transport != gb28181_transport::udp || description_.rtp_port != 0 || description_.rtcp_port != 0 || local_ports_ ||
        description_.address.is_unspecified() || !media_.startup())
    {
        return false;
    }

    auto local_ports = prepare_udp_transports(description_.address);
    if (!local_ports)
    {
        media_.shutdown();
        return false;
    }
    local_ports_ = *local_ports;

    const auto self = shared_from_this();
    boost::asio::spawn(worker_.io(), [self](boost::asio::yield_context yield) { self->run_rtp(yield); }, boost::asio::detached);
    boost::asio::spawn(worker_.io(), [self](boost::asio::yield_context yield) { self->run_rtcp(yield); }, boost::asio::detached);
    boost::asio::spawn(worker_.io(), [self](boost::asio::yield_context yield) { self->run_rtcp_sender(yield); }, boost::asio::detached);

    spdlog::info("gb28181 udp session started stream {} rtp_port {} rtcp_port {}",
                 media_.stream_name(),
                 local_ports_->first,
                 local_ports_->second);
    return true;
}

void gb28181_udp_session::shutdown()
{
    const auto self = shared_from_this();
    boost::asio::post(worker_.io(), [self]() { self->safe_shutdown(); });
}

const std::string& gb28181_udp_session::stream_name() const noexcept { return media_.stream_name(); }

std::optional<port_manager_impl::port_pair> gb28181_udp_session::local_ports() const noexcept { return local_ports_; }

void gb28181_udp_session::run_rtp(boost::asio::yield_context yield)
{
    std::vector<std::uint8_t> buffer(64 * 1024);
    boost::system::error_code error;
    for (;;)
    {
        boost::asio::ip::udp::endpoint endpoint;
        const auto bytes = rtp_transport_.read(buffer, endpoint, yield, error);
        if (error)
        {
            break;
        }

        const auto result = media_.input_rtp(std::span{buffer.data(), bytes});
        if (result == gb28181_rtp_input_result::fatal)
        {
            break;
        }
        if (result == gb28181_rtp_input_result::accepted && !remote_rtp_endpoint_)
        {
            rtp_transport_.connect(endpoint, error);
            if (error)
            {
                break;
            }
            remote_rtp_endpoint_ = endpoint;
        }
    }

    shutdown();
}

void gb28181_udp_session::run_rtcp(boost::asio::yield_context yield)
{
    std::vector<std::uint8_t> buffer(64 * 1024);
    boost::system::error_code error;
    for (;;)
    {
        boost::asio::ip::udp::endpoint endpoint;
        const auto bytes = rtcp_transport_.read(buffer, endpoint, yield, error);
        if (error)
        {
            break;
        }
        if (media_.input_rtcp(std::span{buffer.data(), bytes}) <= 0 || remote_rtcp_endpoint_ || !remote_rtp_endpoint_ ||
            endpoint.address() != remote_rtp_endpoint_->address())
        {
            continue;
        }

        rtcp_transport_.connect(endpoint, error);
        if (error)
        {
            break;
        }
        remote_rtcp_endpoint_ = endpoint;
    }

    shutdown();
}

void gb28181_udp_session::run_rtcp_sender(boost::asio::yield_context yield)
{
    boost::system::error_code error;
    for (;;)
    {
        rtcp_timer_.expires_after(std::chrono::seconds(1));
        rtcp_timer_.async_wait(yield[error]);
        if (error)
        {
            break;
        }

        std::optional<boost::asio::ip::udp::endpoint> target = remote_rtcp_endpoint_;
        if (!target && remote_rtp_endpoint_ && remote_rtp_endpoint_->port() != std::numeric_limits<std::uint16_t>::max())
        {
            target.emplace(remote_rtp_endpoint_->address(), static_cast<std::uint16_t>(remote_rtp_endpoint_->port() + 1U));
        }
        if (!target)
        {
            continue;
        }

        std::array<std::uint8_t, 1500> buffer{};
        const auto bytes = media_.generate_rtcp(buffer);
        if (bytes <= 0)
        {
            continue;
        }
        static_cast<void>(rtcp_transport_.write(std::span{buffer.data(), static_cast<std::size_t>(bytes)}, *target, yield, error));
        if (error)
        {
            break;
        }
    }

    shutdown();
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
    rtp_transport_.shutdown();
    rtcp_transport_.shutdown();
    media_.shutdown();
    if (local_ports_)
    {
        port_manager::instance().release(*local_ports_);
        local_ports_.reset();
    }
    remote_rtp_endpoint_.reset();
    remote_rtcp_endpoint_.reset();
    spdlog::debug("gb28181 udp session shutdown {}", media_.stream_name());
}

}    // namespace media_server
