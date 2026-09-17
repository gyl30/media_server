#include <span>
#include <vector>
#include <cstddef>
#include <utility>

#include <spdlog/spdlog.h>
#include <boost/asio/post.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/cancel_after.hpp>

#include "media/net/worker_context.h"
#include "media/core/stream_registry.h"
#include "media/gb28181/gb28181_event.h"
#include "media/http/signaling_client.h"
#include "media/gb28181/gb28181_tcp_receiver_session.h"

namespace media_server
{
namespace
{
bool remote_disconnect(const boost::system::error_code& error)
{
    return error == boost::asio::error::eof || error == boost::asio::error::connection_reset || error == boost::asio::error::connection_aborted;
}
}    // namespace

gb28181_tcp_receiver_session::gb28181_tcp_receiver_session(worker_context& worker,
                                                           std::string stream_id,
                                                           std::string stream_name,
                                                           gb28181_transport_config config,
                                                           boost::asio::ip::address bind_address,
                                                           std::chrono::milliseconds establishment_timeout)
    : worker_(worker),
      stream_id_(std::move(stream_id)),
      config_(std::move(config)),
      bind_address_(std::move(bind_address)),
      receiver_(worker_, std::move(stream_name), config_.payload_type, config_.ssrc),
      establishment_timeout_(establishment_timeout),
      socket_(worker_.io())
{
}

bool gb28181_tcp_receiver_session::startup()
{
    if (started_ || closed_)
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
            spdlog::error("gb28181 tcp listener startup failed stream {} error {}", receiver_.stream_name(), error.message());
            listener_.reset();
            return false;
        }
    }

    started_ = true;
    const auto self = shared_from_this();
    boost::asio::spawn(worker_.io(), [self](boost::asio::yield_context yield) { self->run(yield); }, boost::asio::detached);
    emit_starting();
    return true;
}

void gb28181_tcp_receiver_session::shutdown()
{
    const auto self = shared_from_this();
    boost::asio::post(worker_.io(), [self]() { self->safe_shutdown(); });
}

std::string_view gb28181_tcp_receiver_session::stream_id() const noexcept { return stream_id_; }

void gb28181_tcp_receiver_session::run(boost::asio::yield_context yield)
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

    if (closed_)
    {
        return;
    }
    if (error)
    {
        if (error != boost::asio::error::operation_aborted)
        {
            spdlog::warn("gb28181 tcp establishment failed stream {} error {}", receiver_.stream_name(), error.message());
        }
        if (runtime_started_)
        {
            runtime_started_ = false;
            runtime_streaming_ = false;
            signaling_client::instance().report(gb28181_event::source_stopped(
                stream_id_,
                receiver_.stream_name(),
                error == boost::asio::error::timed_out ? runtime_end_reason::timeout : runtime_end_reason::runtime_error,
                error == boost::asio::error::timed_out ? "establishment_timeout" : error.message()));
        }
        shutdown();
        return;
    }

    transport_ = std::make_unique<tcp_yield_transport>(std::move(socket_));
    if (!receiver_.startup())
    {
        spdlog::error("gb28181 tcp receiver startup failed stream {}", receiver_.stream_name());
        if (runtime_started_)
        {
            runtime_started_ = false;
            runtime_streaming_ = false;
            signaling_client::instance().report(
                gb28181_event::source_stopped(stream_id_, receiver_.stream_name(), runtime_end_reason::runtime_error, "receiver_startup_failed"));
        }
        shutdown();
        return;
    }

    spdlog::info("gb28181 tcp session started stream {}", receiver_.stream_name());

    std::vector<std::uint8_t> buffer(64 * 1024);
    std::vector<std::uint8_t> input_buffer;
    input_buffer.reserve(buffer.size());
    for (;;)
    {
        const auto read_bytes = transport_->read(buffer, yield, error);
        if (closed_)
        {
            return;
        }
        if (error)
        {
            const auto reason = remote_disconnect(error) ? runtime_end_reason::remote : runtime_end_reason::runtime_error;
            if (runtime_started_)
            {
                runtime_started_ = false;
                runtime_streaming_ = false;
                signaling_client::instance().report(gb28181_event::source_stopped(
                    stream_id_, receiver_.stream_name(), reason, reason == runtime_end_reason::remote ? std::string{} : error.message()));
            }
            shutdown();
            return;
        }

        input_buffer.insert(input_buffer.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(read_bytes));
        std::size_t offset = 0;
        while (input_buffer.size() - offset >= 2U)
        {
            const auto packet_bytes = static_cast<std::size_t>((static_cast<std::uint16_t>(input_buffer[offset]) << 8U) | input_buffer[offset + 1U]);
            if (input_buffer.size() - offset < packet_bytes + 2U)
            {
                break;
            }
            offset += 2U;
            if (packet_bytes != 0U)
            {
                const std::span packet{input_buffer.data() + offset, packet_bytes};
                const auto result = receiver_.receive_rtp(packet);
                if (!runtime_streaming_ && receiver_.recording())
                {
                    emit_streaming();
                }
                if (result == gb28181_rtp_receive_result::fatal)
                {
                    if (runtime_started_)
                    {
                        runtime_started_ = false;
                        runtime_streaming_ = false;
                        signaling_client::instance().report(gb28181_event::source_stopped(
                            stream_id_, receiver_.stream_name(), runtime_end_reason::protocol_error, "media_input_failed"));
                    }
                    shutdown();
                    return;
                }
            }
            offset += packet_bytes;
        }

        if (offset == input_buffer.size())
        {
            input_buffer.clear();
        }
        else if (offset != 0U)
        {
            input_buffer.erase(input_buffer.begin(), input_buffer.begin() + static_cast<std::ptrdiff_t>(offset));
        }
    }
}

void gb28181_tcp_receiver_session::safe_shutdown()
{
    if (closed_)
    {
        return;
    }
    closed_ = true;
    stream_registry::instance().remove_receiver_session(receiver_.stream_name(), *this);
    if (listener_)
    {
        listener_->shutdown();
    }
    boost::system::error_code error;
    socket_.cancel(error);
    socket_.close(error);
    receiver_.shutdown();
    if (transport_)
    {
        transport_->shutdown();
    }
    spdlog::debug("gb28181 tcp session shutdown {}", receiver_.stream_name());
}

void gb28181_tcp_receiver_session::emit_starting()
{
    if (runtime_started_)
    {
        return;
    }
    runtime_started_ = true;
    signaling_client::instance().report(gb28181_event::source_starting(
        stream_id_, receiver_.stream_name(), config_.mode == gb28181_transport::tcp_passive ? "listening" : "connecting"));
}

void gb28181_tcp_receiver_session::emit_streaming()
{
    if (!runtime_started_ || runtime_streaming_ || closed_)
    {
        return;
    }
    runtime_streaming_ = true;
    signaling_client::instance().report(gb28181_event::source_streaming(stream_id_, receiver_.stream_name()));
}

}    // namespace media_server
