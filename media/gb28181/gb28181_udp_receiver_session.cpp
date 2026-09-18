#include <span>
#include <array>
#include <chrono>
#include <limits>
#include <vector>
#include <utility>

#include <spdlog/spdlog.h>
#include <boost/asio/post.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/scope/scope_exit.hpp>

#include "media/net/worker_context.h"
#include "media/core/stream_registry.h"
#include "media/gb28181/gb28181_event.h"
#include "media/gb28181/gb28181_udp_receiver_session.h"

namespace media_server
{

gb28181_udp_receiver_session::gb28181_udp_receiver_session(worker_context& worker,
                                                           std::string stream_id,
                                                           std::string stream_name,
                                                           gb28181_transport_config config,
                                                           boost::asio::ip::address bind_address,
                                                           std::chrono::milliseconds rtcp_interval)
    : worker_(worker),
      stream_id_(std::move(stream_id)),
      config_(std::move(config)),
      bind_address_(std::move(bind_address)),
      receiver_(worker_, std::move(stream_name), config_.payload_type, config_.ssrc),
      rtp_transport_(worker_.io()),
      rtcp_transport_(worker_.io()),
      rtcp_timer_(worker_.io()),
      rtcp_interval_(rtcp_interval)
{
}

std::optional<port_manager::port_pair> gb28181_udp_receiver_session::prepare_udp_transports(boost::asio::ip::address bind_address)
{
    const auto reserved = port_manager::instance().acquire_pair();
    if (!reserved)
    {
        return std::nullopt;
    }

    const auto local_ports = *reserved;
    boost::scope::scope_exit release_ports([&]() { port_manager::instance().release(local_ports); });
    boost::system::error_code network_error;
    rtp_transport_.startup(bind_address, local_ports.first, network_error);
    if (network_error)
    {
        return std::nullopt;
    }

    rtcp_transport_.startup(std::move(bind_address), local_ports.second, network_error);
    if (network_error)
    {
        rtp_transport_.shutdown();
        return std::nullopt;
    }

    release_ports.set_active(false);
    return local_ports;
}

bool gb28181_udp_receiver_session::startup()
{
    if (started_ || closed_ || config_.mode != gb28181_transport::udp || local_ports_ || bind_address_.is_unspecified() || !receiver_.startup())
    {
        return false;
    }

    auto local_ports = prepare_udp_transports(bind_address_);
    if (!local_ports)
    {
        receiver_.shutdown();
        return false;
    }
    local_ports_ = *local_ports;
    started_ = true;

    const auto self = shared_from_this();
    boost::asio::spawn(worker_.io(), [self](boost::asio::yield_context yield) { self->run_rtp(yield); }, boost::asio::detached);
    boost::asio::spawn(worker_.io(), [self](boost::asio::yield_context yield) { self->run_rtcp(yield); }, boost::asio::detached);
    schedule_rtcp();

    spdlog::info(
        "gb28181 udp session started stream {} rtp_port {} rtcp_port {}", receiver_.stream_name(), local_ports_->first, local_ports_->second);
    gb28181_event::report_source(event_state::starting, stream_id_, receiver_.stream_name(), "listening");
    return true;
}

void gb28181_udp_receiver_session::shutdown()
{
    const auto self = shared_from_this();
    boost::asio::post(worker_.io(), [self]() { self->safe_shutdown(); });
}

std::string_view gb28181_udp_receiver_session::stream_id() const noexcept { return stream_id_; }

std::optional<port_manager::port_pair> gb28181_udp_receiver_session::local_ports() const noexcept { return local_ports_; }

void gb28181_udp_receiver_session::run_rtp(boost::asio::yield_context yield)
{
    std::vector<std::uint8_t> buffer(64 * 1024);
    boost::system::error_code error;
    for (;;)
    {
        boost::asio::ip::udp::endpoint endpoint;
        const auto bytes = rtp_transport_.read(buffer, endpoint, yield, error);
        if (closed_)
        {
            return;
        }
        if (error)
        {
            if (started_)
            {
                gb28181_event::report_source(event_state::runtime_error, stream_id_, receiver_.stream_name(), {}, error.message());
            }
            shutdown();
            return;
        }

        const bool was_recording = receiver_.recording();
        const auto result = receiver_.receive_rtp(std::span{buffer.data(), bytes});
        if (!was_recording && receiver_.recording())
        {
            gb28181_event::report_source(event_state::streaming, stream_id_, receiver_.stream_name(), "streaming");
        }
        if (result == gb28181_rtp_receive_result::fatal)
        {
            if (started_)
            {
                gb28181_event::report_source(event_state::protocol_error, stream_id_, receiver_.stream_name(), {}, "media_input_failed");
            }
            shutdown();
            return;
        }
        if (result == gb28181_rtp_receive_result::accepted && !remote_rtp_endpoint_)
        {
            rtp_transport_.connect(endpoint, error);
            if (error)
            {
                if (started_)
                {
                    gb28181_event::report_source(event_state::runtime_error, stream_id_, receiver_.stream_name(), {}, error.message());
                }
                shutdown();
                return;
            }
            remote_rtp_endpoint_ = endpoint;
        }
    }
}

void gb28181_udp_receiver_session::run_rtcp(boost::asio::yield_context yield)
{
    std::vector<std::uint8_t> buffer(64 * 1024);
    boost::system::error_code error;
    for (;;)
    {
        boost::asio::ip::udp::endpoint endpoint;
        const auto bytes = rtcp_transport_.read(buffer, endpoint, yield, error);
        if (closed_)
        {
            return;
        }
        if (error)
        {
            if (started_)
            {
                gb28181_event::report_source(event_state::runtime_error, stream_id_, receiver_.stream_name(), {}, error.message());
            }
            shutdown();
            return;
        }
        if (receiver_.receive_rtcp(std::span{buffer.data(), bytes}) <= 0 || remote_rtcp_endpoint_ || !remote_rtp_endpoint_ ||
            endpoint.address() != remote_rtp_endpoint_->address())
        {
            continue;
        }

        rtcp_transport_.connect(endpoint, error);
        if (error)
        {
            if (started_)
            {
                gb28181_event::report_source(event_state::runtime_error, stream_id_, receiver_.stream_name(), {}, error.message());
            }
            shutdown();
            return;
        }
        remote_rtcp_endpoint_ = endpoint;
    }
}

void gb28181_udp_receiver_session::schedule_rtcp()
{
    if (closed_ || !local_ports_)
    {
        return;
    }

    rtcp_timer_.expires_after(rtcp_interval_);
    const auto self = shared_from_this();
    rtcp_timer_.async_wait(
        [self](const boost::system::error_code& error)
        {
            if (error || self->closed_)
            {
                return;
            }

            std::optional<boost::asio::ip::udp::endpoint> target = self->remote_rtcp_endpoint_;
            if (!target && self->remote_rtp_endpoint_ && self->remote_rtp_endpoint_->port() != std::numeric_limits<std::uint16_t>::max())
            {
                target.emplace(self->remote_rtp_endpoint_->address(), static_cast<std::uint16_t>(self->remote_rtp_endpoint_->port() + 1U));
            }
            if (!target)
            {
                self->schedule_rtcp();
                return;
            }

            std::array<std::uint8_t, 1500> buffer{};
            const auto bytes = self->receiver_.generate_rtcp(buffer);
            if (bytes <= 0)
            {
                self->schedule_rtcp();
                return;
            }

            auto packet = std::make_shared<std::vector<std::uint8_t>>(buffer.begin(), buffer.begin() + bytes);
            boost::asio::spawn(
                self->worker_.io(),
                [self, packet, target = *target](boost::asio::yield_context yield)
                {
                    if (self->closed_)
                    {
                        return;
                    }
                    boost::system::error_code write_error;
                    self->rtcp_transport_.write(std::span<const std::uint8_t>{packet->data(), packet->size()}, target, yield, write_error);
                    if (self->closed_)
                    {
                        return;
                    }
                    if (write_error)
                    {
                        if (self->started_)
                        {
                            gb28181_event::report_source(
                                event_state::runtime_error, self->stream_id_, self->receiver_.stream_name(), {}, write_error.message());
                        }
                        self->shutdown();
                        return;
                    }
                    self->schedule_rtcp();
                },
                boost::asio::detached);
        });
}

void gb28181_udp_receiver_session::safe_shutdown()
{
    if (closed_)
    {
        return;
    }
    closed_ = true;
    if (started_)
    {
        gb28181_event::report_source(event_state::stopped, stream_id_, receiver_.stream_name());
    }
    started_ = false;
    stream_registry::instance().remove_receiver_session(receiver_.stream_name(), *this);
    rtcp_timer_.cancel();
    rtp_transport_.shutdown();
    rtcp_transport_.shutdown();
    receiver_.shutdown();
    if (local_ports_)
    {
        port_manager::instance().release(*local_ports_);
    }
    local_ports_.reset();
    remote_rtp_endpoint_.reset();
    remote_rtcp_endpoint_.reset();
    spdlog::debug("gb28181 udp session shutdown {}", receiver_.stream_name());
}

}    // namespace media_server
