#include <array>
#include <chrono>
#include <limits>
#include <span>
#include <vector>
#include <utility>

#include <spdlog/spdlog.h>
#include <boost/asio/detached.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/spawn.hpp>

#include "media/core/stream_registry.h"
#include "media/net/worker_context.h"
#include "media/gb28181/gb28181_udp_receiver_session.h"

namespace media_server
{

gb28181_udp_receiver_session::gb28181_udp_receiver_session(worker_context& worker,
                                                           std::string stream_id,
                                                           std::string stream_name,
                                                           gb28181_transport_config config,
                                                           boost::asio::ip::address bind_address,
                                                           std::chrono::milliseconds rtcp_interval,
                                                           runtime_event_emitter_ptr runtime_events)
    : worker_(worker),
      stream_id_(std::move(stream_id)),
      config_(std::move(config)),
      bind_address_(std::move(bind_address)),
      receiver_(worker_, std::move(stream_name), config_.payload_type, config_.ssrc),
      rtp_transport_(worker_.io()),
      rtcp_transport_(worker_.io()),
      rtcp_timer_(worker_.io()),
      rtcp_interval_(rtcp_interval),
      runtime_events_(std::move(runtime_events))
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

bool gb28181_udp_receiver_session::startup()
{
    if (started_ || ending_ || closed_ || config_.mode != gb28181_transport::udp || local_ports_ || bind_address_.is_unspecified() ||
        !receiver_.startup())
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

    spdlog::info("gb28181 udp session started stream {} rtp_port {} rtcp_port {}",
                 receiver_.stream_name(),
                 local_ports_->first,
                 local_ports_->second);
    emit_starting();
    return true;
}

void gb28181_udp_receiver_session::shutdown(runtime_end_reason reason, std::string error)
{
    const auto self = shared_from_this();
    boost::asio::dispatch(worker_.io(),
                          [self, reason, error = std::move(error)]() mutable
                          {
                              if (self->ending_ || self->closed_)
                              {
                                  return;
                              }
                              self->ending_ = true;
                              self->end_reason_ = reason;
                              self->end_error_ = std::move(error);
                              boost::asio::post(self->worker_.io(), [self]() { self->safe_shutdown(); });
                          });
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
        if (ending_ || closed_)
        {
            return;
        }
        if (error)
        {
            shutdown(runtime_end_reason::runtime_error, error.message());
            return;
        }

        const auto result = receiver_.receive_rtp(std::span{buffer.data(), bytes});
        if (!runtime_streaming_ && receiver_.recording())
        {
            emit_streaming();
        }
        if (result == gb28181_rtp_receive_result::fatal)
        {
            shutdown(runtime_end_reason::protocol_error, "media_input_failed");
            return;
        }
        if (result == gb28181_rtp_receive_result::accepted && !remote_rtp_endpoint_)
        {
            rtp_transport_.connect(endpoint, error);
            if (error)
            {
                shutdown(runtime_end_reason::runtime_error, error.message());
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
        if (ending_ || closed_)
        {
            return;
        }
        if (error)
        {
            shutdown(runtime_end_reason::runtime_error, error.message());
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
            shutdown(runtime_end_reason::runtime_error, error.message());
            return;
        }
        remote_rtcp_endpoint_ = endpoint;
    }
}

void gb28181_udp_receiver_session::schedule_rtcp()
{
    if (ending_ || closed_ || !local_ports_)
    {
        return;
    }

    rtcp_timer_.expires_after(rtcp_interval_);
    const auto self = shared_from_this();
    rtcp_timer_.async_wait(
        [self](const boost::system::error_code& error)
        {
            if (error || self->ending_ || self->closed_)
            {
                return;
            }

            std::optional<boost::asio::ip::udp::endpoint> target = self->remote_rtcp_endpoint_;
            if (!target && self->remote_rtp_endpoint_ &&
                self->remote_rtp_endpoint_->port() != std::numeric_limits<std::uint16_t>::max())
            {
                target.emplace(self->remote_rtp_endpoint_->address(),
                               static_cast<std::uint16_t>(self->remote_rtp_endpoint_->port() + 1U));
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
                    if (self->ending_ || self->closed_)
                    {
                        return;
                    }
                    boost::system::error_code write_error;
                    static_cast<void>(
                        self->rtcp_transport_.write(std::span<const std::uint8_t>{packet->data(), packet->size()}, target, yield, write_error));
                    if (self->ending_ || self->closed_)
                    {
                        return;
                    }
                    if (write_error)
                    {
                        self->shutdown(runtime_end_reason::runtime_error, write_error.message());
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
    emit_stopped();
    spdlog::debug("gb28181 udp session shutdown {}", receiver_.stream_name());
}

void gb28181_udp_receiver_session::emit_starting()
{
    if (runtime_started_)
    {
        return;
    }
    runtime_started_ = true;
    if (runtime_events_)
    {
        runtime_events_->emit(runtime_event{
            .type = runtime_event_type::source_started,
            .stream_id = stream_id_,
            .stream_name = receiver_.stream_name(),
            .direction = runtime_direction::input,
            .protocol = runtime_protocol::gb28181,
            .state = runtime_state::starting,
            .stage = "listening",
        });
    }
}

void gb28181_udp_receiver_session::emit_streaming()
{
    if (!runtime_started_ || runtime_streaming_ || ending_ || closed_)
    {
        return;
    }
    runtime_streaming_ = true;
    if (runtime_events_)
    {
        runtime_events_->emit(runtime_event{
            .type = runtime_event_type::source_started,
            .stream_id = stream_id_,
            .stream_name = receiver_.stream_name(),
            .direction = runtime_direction::input,
            .protocol = runtime_protocol::gb28181,
            .state = runtime_state::streaming,
            .stage = "streaming",
        });
    }
}

void gb28181_udp_receiver_session::emit_stopped()
{
    if (!runtime_started_)
    {
        return;
    }
    runtime_started_ = false;
    runtime_streaming_ = false;
    if (runtime_events_)
    {
        runtime_events_->emit(runtime_event{
            .type = terminal_event_type(runtime_event_type::source_stopped, end_reason_),
            .stream_id = stream_id_,
            .stream_name = receiver_.stream_name(),
            .direction = runtime_direction::input,
            .protocol = runtime_protocol::gb28181,
            .state = runtime_state::stopped,
            .end_reason = end_reason_,
            .error = end_error_.empty() ? std::nullopt : std::optional<std::string>{end_error_},
        });
    }
}

}    // namespace media_server
