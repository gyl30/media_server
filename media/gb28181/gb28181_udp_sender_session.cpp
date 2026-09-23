#include <span>
#include <array>
#include <chrono>
#include <utility>

#include <spdlog/spdlog.h>
#include <boost/asio/post.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/scope/scope_exit.hpp>

#include "media/net/worker_context.h"
#include "media/core/stream_registry.h"
#include "media/gb28181/gb28181_event.h"
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
                                                       std::size_t max_write_queue_bytes)
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
    if (closed_ || local_ports_ || sender_ || !stream_ || config_.mode != gb28181_transport::udp || config_.remote_address.is_unspecified() ||
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
    shutdown_subscription_ = worker_.subscribe_shutdown([self = shared_from_this()]() { self->safe_shutdown(); });
    if (!shutdown_subscription_)
    {
        shutdown_udp_transports();
        return false;
    }

    if (rtcp_enabled_)
    {
        rtp_event_t handler{};
        rtcp_sender_ = rtp_create(&handler, nullptr, config_.ssrc, 0, 90'000, 2 * 1024 * 1024, 1);
        if (rtcp_sender_ == nullptr)
        {
            shutdown_subscription_.reset();
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
        [self]()
        {
            if (self->sender_)
            {
                gb28181_event::report_output(event_state::remote_closed, self->stream_id_, self->stream_name_);
            }
            self->shutdown();
        },
        [self]()
        {
            if (self->sender_)
            {
                gb28181_event::report_output(event_state::runtime_error, self->stream_id_, self->stream_name_, {}, "sender_mux_failed");
            }
            self->shutdown();
        });
    if (!sender_->startup())
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
                 config_.remote_address.to_string(),
                 config_.remote_rtp_port,
                 config_.remote_address.to_string(),
                 config_.remote_rtcp_port,
                 rtcp_enabled_);
    gb28181_event::report_output(event_state::starting, stream_id_, stream_name_);
    return true;
}

void gb28181_udp_sender_session::shutdown()
{
    const auto self = shared_from_this();
    boost::asio::post(worker_.io(), [self]() { self->safe_shutdown(); });
}

std::string_view gb28181_udp_sender_session::stream_id() const noexcept { return stream_id_; }

void gb28181_udp_sender_session::run_rtp_write(boost::asio::yield_context yield)
{
    for (;;)
    {
        if (closed_ || !local_ports_ || write_queue_.empty())
        {
            return;
        }

        const auto data = write_queue_.front();
        boost::system::error_code error;
        rtp_transport_.write(std::span<const std::uint8_t>{data->data(), data->size()}, remote_rtp_endpoint_, yield, error);
        if (closed_ || !sender_ || error == boost::asio::error::operation_aborted)
        {
            return;
        }
        if (error)
        {
            sender_->shutdown();
            gb28181_event::report_output(event_state::runtime_error, stream_id_, stream_name_, {}, error.message());
            shutdown();
            return;
        }

        queued_write_bytes_ -= data->size();
        write_queue_.pop_front();
    }
}

void gb28181_udp_sender_session::schedule_rtcp()
{
    if (closed_ || !sender_ || rtcp_sender_ == nullptr)
    {
        return;
    }

    rtcp_timer_.expires_after(rtcp_interval_);
    const auto self = shared_from_this();
    rtcp_timer_.async_wait(
        [self](const boost::system::error_code& error)
        {
            if (error || self->closed_ || !self->sender_ || self->rtcp_sender_ == nullptr)
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
            self->worker_.spawn(
                [self, packet](boost::asio::yield_context yield)
                {
                    if (self->closed_)
                    {
                        return;
                    }
                    boost::system::error_code write_error;
                    self->rtcp_transport_.write(
                        std::span<const std::uint8_t>{packet->data(), packet->size()}, self->remote_rtcp_endpoint_, yield, write_error);
                    if (self->closed_ || !self->sender_ || write_error == boost::asio::error::operation_aborted)
                    {
                        return;
                    }
                    if (write_error)
                    {
                        self->sender_->shutdown();
                        gb28181_event::report_output(event_state::runtime_error, self->stream_id_, self->stream_name_, {}, write_error.message());
                        self->shutdown();
                        return;
                    }
                    self->schedule_rtcp();
                });
        });
}

void gb28181_udp_sender_session::send_packet(std::vector<std::uint8_t> packet)
{
    if (closed_ || !sender_ || !local_ports_)
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
        sender_->shutdown();
        gb28181_event::report_output(event_state::runtime_error, stream_id_, stream_name_, {}, "rtcp_sender_input_failed");
        shutdown();
        return;
    }

    const bool start_write = write_queue_.empty();
    queued_write_bytes_ += packet.size();
    write_queue_.push_back(std::make_shared<std::vector<std::uint8_t>>(std::move(packet)));
    if (!media_started_)
    {
        media_started_ = true;
        gb28181_event::report_output(event_state::streaming, stream_id_, stream_name_, "streaming");
    }
    if (start_write)
    {
        const auto self = shared_from_this();
        worker_.spawn([self](boost::asio::yield_context yield) { self->run_rtp_write(yield); });
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
    shutdown_subscription_.reset();
    media_started_ = false;
    stream_registry::instance().remove_sender_session(stream_name_, sender_id_, *this);
    rtcp_timer_.cancel();
    if (sender_)
    {
        gb28181_event::report_output(event_state::stopped, stream_id_, stream_name_);
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
}

}    // namespace media_server
