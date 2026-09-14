#include <array>
#include <chrono>
#include <span>
#include <utility>

#include <spdlog/spdlog.h>
#include <boost/asio/detached.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/spawn.hpp>

#include "media/net/worker_context.h"
#include "media/core/stream_registry.h"
#include "media/gb28181/gb28181_rtp_sender.h"
#include "media/gb28181/gb28181_udp_sender_session.h"

extern "C"
{
#include "rtp.h"
}

namespace media_server
{
gb28181_udp_sender_session::gb28181_udp_sender_session(worker_context& worker,
                                                       std::string stream_id,
                                                       std::shared_ptr<media_stream> stream,
                                                       gb28181_transport_config config,
                                                       boost::asio::ip::address bind_address,
                                                       std::string sender_id,
                                                       bool rtcp_enabled,
                                                       std::chrono::milliseconds rtcp_interval,
                                                       std::size_t max_write_queue_bytes,
                                                       runtime_event_emitter_ptr runtime_events)
    : worker_(worker),
      stream_id_(std::move(stream_id)),
      stream_(std::move(stream)),
      stream_name_(stream_ ? stream_->name() : std::string{}),
      sender_id_(std::move(sender_id)),
      config_(std::move(config)),
      bind_address_(std::move(bind_address)),
      remote_rtp_endpoint_(config_.remote_address, config_.remote_rtp_port),
      remote_rtcp_endpoint_(config_.remote_address, config_.remote_rtcp_port),
      rtp_transport_(worker_.io()),
      rtcp_transport_(worker_.io()),
      rtcp_timer_(worker_.io()),
      rtcp_interval_(rtcp_interval),
      max_write_queue_bytes_(max_write_queue_bytes),
      runtime_events_(std::move(runtime_events)),
      rtcp_enabled_(rtcp_enabled)
{
}

std::optional<port_manager::port_pair> gb28181_udp_sender_session::prepare_udp_transports(boost::asio::ip::address bind_address)
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

void gb28181_udp_sender_session::shutdown_udp_transports()
{
    rtp_transport_.shutdown();
    rtcp_transport_.shutdown();
    if (local_ports_)
    {
        port_manager::instance().release(*local_ports_);
        local_ports_.reset();
    }
}

bool gb28181_udp_sender_session::startup()
{
    if (ending_ || closed_ || runtime_started_ || local_ports_ || sender_ || !stream_ || config_.mode != gb28181_transport::udp ||
        config_.remote_address.is_unspecified() || bind_address_.is_unspecified())
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
        rtcp_sender_ = rtp_create(&handler, nullptr, config_.ssrc, 0, 90'000, 2 * 1024 * 1024, 1);
        if (rtcp_sender_ == nullptr)
        {
            shutdown_udp_transports();
            return false;
        }
    }

    const auto self = shared_from_this();
    sender_ = std::make_shared<gb28181_rtp_sender>(
        worker_,
        stream_,
        config_.payload_type,
        config_.ssrc,
        [self](std::vector<std::uint8_t> packet) { self->send_packet(std::move(packet)); },
        [self]() { self->shutdown(runtime_end_reason::remote); },
        [self]() { self->shutdown(runtime_end_reason::runtime_error, "sender_mux_failed"); });
    if (!sender_->startup())
    {
        sender_->shutdown();
        sender_.reset();
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
                 config_.remote_address.to_string(),
                 config_.remote_rtp_port,
                 config_.remote_address.to_string(),
                 config_.remote_rtcp_port,
                 rtcp_enabled_);
    emit_starting();
    return true;
}

void gb28181_udp_sender_session::shutdown(runtime_end_reason reason, std::string error)
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

std::string_view gb28181_udp_sender_session::stream_id() const noexcept { return stream_id_; }

void gb28181_udp_sender_session::run_rtp_write(boost::asio::yield_context yield)
{
    for (;;)
    {
        if (ending_ || closed_ || !local_ports_ || write_queue_.empty())
        {
            return;
        }

        const auto data = write_queue_.front();
        boost::system::error_code error;
        static_cast<void>(
            rtp_transport_.write(std::span<const std::uint8_t>{data->data(), data->size()}, remote_rtp_endpoint_, yield, error));
        if (error)
        {
            shutdown(runtime_end_reason::runtime_error, error.message());
            return;
        }
        if (!local_ports_)
        {
            return;
        }

        queued_write_bytes_ -= data->size();
        write_queue_.pop_front();
    }
}

void gb28181_udp_sender_session::schedule_rtcp()
{
    if (ending_ || closed_ || rtcp_sender_ == nullptr)
    {
        return;
    }

    rtcp_timer_.expires_after(rtcp_interval_);
    const auto self = shared_from_this();
    rtcp_timer_.async_wait(
        [self](const boost::system::error_code& error)
        {
            if (error || self->ending_ || self->closed_ || self->rtcp_sender_ == nullptr)
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

            auto packet = std::make_shared<std::vector<std::uint8_t>>(buffer.begin(), buffer.begin() + bytes);
            boost::asio::spawn(
                self->worker_.io(),
                [self, packet](boost::asio::yield_context yield)
                {
                    if (self->ending_ || self->closed_)
                    {
                        return;
                    }
                    boost::system::error_code write_error;
                    static_cast<void>(self->rtcp_transport_.write(
                        std::span<const std::uint8_t>{packet->data(), packet->size()}, self->remote_rtcp_endpoint_, yield, write_error));
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

void gb28181_udp_sender_session::send_packet(std::vector<std::uint8_t> packet)
{
    if (ending_ || closed_ || !local_ports_)
    {
        return;
    }
    if (packet.size() > max_write_queue_bytes_ || queued_write_bytes_ > max_write_queue_bytes_ - packet.size())
    {
        spdlog::warn("gb28181 udp write queue full stream {} sender {} queued {} limit {} dropped {}",
                     stream_->name(),
                     sender_id_,
                     queued_write_bytes_,
                     max_write_queue_bytes_,
                     packet.size());
        return;
    }
    if (rtcp_sender_ != nullptr && rtp_onsend(rtcp_sender_, packet.data(), static_cast<int>(packet.size())) != 0)
    {
        shutdown(runtime_end_reason::runtime_error, "rtcp_sender_input_failed");
        return;
    }

    const bool start_write = write_queue_.empty();
    queued_write_bytes_ += packet.size();
    write_queue_.push_back(std::make_shared<std::vector<std::uint8_t>>(std::move(packet)));
    if (!runtime_streaming_)
    {
        emit_streaming();
    }
    if (ending_ || closed_)
    {
        return;
    }
    if (start_write)
    {
        const auto self = shared_from_this();
        boost::asio::spawn(worker_.io(), [self](boost::asio::yield_context yield) { self->run_rtp_write(yield); }, boost::asio::detached);
    }

    if (rtcp_sender_ != nullptr && !rtcp_started_)
    {
        rtcp_started_ = true;
        schedule_rtcp();
    }
}

void gb28181_udp_sender_session::safe_shutdown()
{
    if (closed_)
    {
        return;
    }
    closed_ = true;
    stream_registry::instance().remove_sender_session(stream_name_, sender_id_, *this);
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
    rtcp_started_ = false;
    spdlog::debug("gb28181 udp sender shutdown {} sender {}", stream_name_, sender_id_);
    stream_.reset();
    emit_stopped();
}

void gb28181_udp_sender_session::emit_starting()
{
    if (runtime_started_)
    {
        return;
    }
    runtime_started_ = true;
    if (runtime_events_)
    {
        runtime_events_->emit(runtime_event{
            .type = runtime_event_type::output_started,
            .stream_id = stream_id_,
            .stream_name = stream_name_,
            .direction = runtime_direction::output,
            .protocol = runtime_protocol::gb28181,
            .state = runtime_state::starting,
        });
    }
}

void gb28181_udp_sender_session::emit_streaming()
{
    if (!runtime_started_ || runtime_streaming_ || ending_ || closed_)
    {
        return;
    }
    runtime_streaming_ = true;
    if (runtime_events_)
    {
        runtime_events_->emit(runtime_event{
            .type = runtime_event_type::output_started,
            .stream_id = stream_id_,
            .stream_name = stream_name_,
            .direction = runtime_direction::output,
            .protocol = runtime_protocol::gb28181,
            .state = runtime_state::streaming,
            .stage = "streaming",
        });
    }
}

void gb28181_udp_sender_session::emit_stopped()
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
            .type = terminal_event_type(runtime_event_type::output_stopped, end_reason_),
            .stream_id = stream_id_,
            .stream_name = stream_name_,
            .direction = runtime_direction::output,
            .protocol = runtime_protocol::gb28181,
            .state = runtime_state::stopped,
            .end_reason = end_reason_,
            .error = end_error_.empty() ? std::nullopt : std::optional<std::string>{end_error_},
        });
    }
}

}    // namespace media_server
