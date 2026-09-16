#include <chrono>
#include <limits>
#include <vector>
#include <utility>
#include <algorithm>

#include <spdlog/spdlog.h>
#include <boost/asio/post.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/cancel_after.hpp>

#include "media/net/worker_context.h"
#include "media/core/stream_registry.h"
#include "media/gb28181/gb28181_event.h"
#include "media/http/signaling_client.h"
#include "media/gb28181/gb28181_rtp_sender.h"
#include "media/gb28181/gb28181_tcp_sender_session.h"

namespace media_server
{
gb28181_tcp_sender_session::gb28181_tcp_sender_session(worker_context& worker,
                                                       std::string stream_id,
                                                       std::shared_ptr<media_stream> stream,
                                                       std::string sender_id,
                                                       gb28181_transport_config config,
                                                       boost::asio::ip::address bind_address,
                                                       std::chrono::milliseconds establishment_timeout,
                                                       std::size_t max_write_queue_bytes)
    : worker_(worker),
      stream_id_(std::move(stream_id)),
      stream_(std::move(stream)),
      stream_name_(stream_ ? stream_->name() : std::string{}),
      sender_id_(std::move(sender_id)),
      config_(std::move(config)),
      bind_address_(std::move(bind_address)),
      establishment_timeout_(establishment_timeout),
      max_write_queue_bytes_(max_write_queue_bytes),
      socket_(worker_.io())
{
}

bool gb28181_tcp_sender_session::startup()
{
    if (ending_ || closed_ || runtime_started_ || !stream_)
    {
        return false;
    }
    if (config_.mode == gb28181_transport::tcp_passive)
    {
        listener_ = std::make_unique<tcp_listener>(worker_.io(), config_.listen_port, bind_address_);
        boost::system::error_code error;
        listener_->startup(error);
        if (error)
        {
            spdlog::error("gb28181 tcp sender listener startup failed stream {} sender {} error {}", stream_->name(), sender_id_, error.message());
            listener_.reset();
            return false;
        }
    }

    const auto self = shared_from_this();
    boost::asio::spawn(worker_.io(), [self](boost::asio::yield_context yield) { self->run(yield); }, boost::asio::detached);
    emit_starting();
    return true;
}

void gb28181_tcp_sender_session::run(boost::asio::yield_context yield)
{
    boost::system::error_code error;
    if (config_.mode == gb28181_transport::tcp_passive)
    {
        listener_->accept(socket_, establishment_timeout_, yield, error);
        listener_->shutdown();
        listener_.reset();
    }
    else
    {
        socket_.async_connect(boost::asio::ip::tcp::endpoint{config_.remote_address, config_.remote_port},
                              boost::asio::cancel_after(establishment_timeout_, yield[error]));
        if (error == boost::asio::error::operation_aborted && socket_.is_open())
        {
            error = boost::asio::error::timed_out;
        }
    }

    if (ending_ || closed_)
    {
        return;
    }
    if (error)
    {
        if (error != boost::asio::error::operation_aborted)
        {
            spdlog::warn("gb28181 tcp sender establishment failed stream {} sender {} error {}", stream_->name(), sender_id_, error.message());
        }
        shutdown(error == boost::asio::error::timed_out ? runtime_end_reason::timeout : runtime_end_reason::runtime_error,
                 error == boost::asio::error::timed_out ? "establishment_timeout" : error.message());
        return;
    }

    transport_ = std::make_unique<tcp_yield_transport>(std::move(socket_));
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
        shutdown(runtime_end_reason::runtime_error, "sender_startup_failed");
        return;
    }

    spdlog::info("gb28181 tcp sender started stream {} sender {}", stream_->name(), sender_id_);

    std::vector<std::uint8_t> buffer(64 * 1024);
    for (;;)
    {
        static_cast<void>(transport_->read(buffer, yield, error));
        if (error)
        {
            break;
        }
    }

    if (ending_ || closed_)
    {
        return;
    }
    const auto reason =
        error == boost::asio::error::eof || error == boost::asio::error::connection_reset || error == boost::asio::error::connection_aborted
            ? runtime_end_reason::remote
            : runtime_end_reason::runtime_error;
    shutdown(reason, reason == runtime_end_reason::remote ? std::string{} : error.message());
}

void gb28181_tcp_sender_session::shutdown(runtime_end_reason reason, std::string error)
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

std::string_view gb28181_tcp_sender_session::stream_id() const noexcept { return stream_id_; }

void gb28181_tcp_sender_session::run_write(boost::asio::yield_context yield)
{
    for (;;)
    {
        if (ending_ || closed_ || write_queue_.empty())
        {
            return;
        }

        const auto data = write_queue_.front();
        boost::system::error_code error;
        static_cast<void>(transport_->write(*data, yield, error));
        if (error)
        {
            shutdown(runtime_end_reason::runtime_error, error.message());
            return;
        }
        if (closed_)
        {
            return;
        }

        queued_write_bytes_ -= data->size();
        write_queue_.pop_front();
    }
}

void gb28181_tcp_sender_session::send_packet(std::vector<std::uint8_t> packet)
{
    if (ending_ || closed_ || !transport_)
    {
        return;
    }
    if (packet.size() > std::numeric_limits<std::uint16_t>::max())
    {
        shutdown(runtime_end_reason::runtime_error, "packet_too_large");
        return;
    }

    const auto frame_bytes = packet.size() + 2U;
    if (frame_bytes > max_write_queue_bytes_ || queued_write_bytes_ > max_write_queue_bytes_ - frame_bytes)
    {
        shutdown(runtime_end_reason::runtime_error, "write_queue_overflow");
        return;
    }

    auto frame = std::make_shared<std::vector<std::uint8_t>>(frame_bytes);
    const auto length = static_cast<std::uint16_t>(packet.size());
    (*frame)[0] = static_cast<std::uint8_t>(length >> 8U);
    (*frame)[1] = static_cast<std::uint8_t>(length & 0xffU);
    std::copy(packet.begin(), packet.end(), frame->begin() + 2);

    const bool start_write = write_queue_.empty();
    queued_write_bytes_ += frame_bytes;
    write_queue_.push_back(std::move(frame));
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
        boost::asio::spawn(worker_.io(), [self](boost::asio::yield_context write_yield) { self->run_write(write_yield); }, boost::asio::detached);
    }
}

void gb28181_tcp_sender_session::safe_shutdown()
{
    if (closed_)
    {
        return;
    }
    closed_ = true;
    stream_registry::instance().remove_sender_session(stream_name_, sender_id_, *this);
    if (listener_)
    {
        listener_->shutdown();
    }
    boost::system::error_code error;
    socket_.cancel(error);
    socket_.close(error);
    if (sender_)
    {
        sender_->shutdown();
        sender_.reset();
    }
    if (transport_)
    {
        transport_->shutdown();
    }
    spdlog::debug("gb28181 tcp sender shutdown {} sender {}", stream_name_, sender_id_);
    stream_.reset();
    emit_stopped();
}

void gb28181_tcp_sender_session::emit_starting()
{
    if (runtime_started_)
    {
        return;
    }
    runtime_started_ = true;
    signaling_client::instance().report(
        gb28181_event::output_starting(stream_id_, stream_name_, config_.mode == gb28181_transport::tcp_passive ? "listening" : "connecting"));
}

void gb28181_tcp_sender_session::emit_streaming()
{
    if (!runtime_started_ || runtime_streaming_ || ending_ || closed_)
    {
        return;
    }
    runtime_streaming_ = true;
    signaling_client::instance().report(gb28181_event::output_streaming(stream_id_, stream_name_));
}

void gb28181_tcp_sender_session::emit_stopped()
{
    if (!runtime_started_)
    {
        return;
    }
    runtime_started_ = false;
    runtime_streaming_ = false;
    signaling_client::instance().report(gb28181_event::output_stopped(stream_id_, stream_name_, end_reason_, end_error_));
}

}    // namespace media_server
