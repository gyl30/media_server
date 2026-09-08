#include <chrono>
#include <limits>
#include <utility>
#include <algorithm>
#include <vector>

#include <spdlog/spdlog.h>
#include <boost/asio/cancel_after.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/post.hpp>

#include "media/core/stream_registry.h"
#include "media/gb28181/gb28181_rtp_sender.h"
#include "media/net/worker_context.h"
#include "media/gb28181/gb28181_tcp_sender_session.h"

namespace media_server
{
gb28181_tcp_sender_session::gb28181_tcp_sender_session(worker_context& worker,
                                                       std::weak_ptr<media_stream> stream,
                                                       std::string stream_name,
                                                       std::string sender_id,
                                                       gb28181_transport_config config,
                                                       boost::asio::ip::address bind_address,
                                                       std::chrono::milliseconds establishment_timeout,
                                                       std::size_t max_write_queue_bytes)
    : worker_(worker),
      stream_(std::move(stream)),
      stream_name_(std::move(stream_name)),
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
    if (config_.mode == gb28181_transport::tcp_passive)
    {
        listener_ = std::make_unique<tcp_listener>(worker_.io(), config_.listen_port, bind_address_);
        boost::system::error_code error;
        listener_->startup(error);
        if (error)
        {
            spdlog::error("gb28181 tcp sender listener startup failed stream {} sender {} error {}",
                          stream_name_,
                          sender_id_,
                          error.message());
            listener_.reset();
            return false;
        }
    }

    const auto self = shared_from_this();
    boost::asio::spawn(worker_.io(), [self](boost::asio::yield_context yield) { self->run(yield); }, boost::asio::detached);
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

    if (error)
    {
        if (error != boost::asio::error::operation_aborted)
        {
            spdlog::warn("gb28181 tcp sender establishment failed stream {} sender {} error {}", stream_name_, sender_id_, error.message());
        }
        shutdown();
        return;
    }

    const auto stream = stream_.lock();
    if (!stream)
    {
        shutdown();
        return;
    }

    transport_ = std::make_unique<tcp_yield_transport>(std::move(socket_));
    const auto weak = weak_from_this();
    sender_ = std::make_shared<gb28181_rtp_sender>(
        worker_,
        stream,
        config_.payload_type,
        config_.ssrc,
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
    if (!sender_->startup())
    {
        shutdown();
        return;
    }

    spdlog::info("gb28181 tcp sender started stream {} sender {}", stream_name_, sender_id_);

    std::vector<std::uint8_t> buffer(64 * 1024);
    for (;;)
    {
        static_cast<void>(transport_->read(buffer, yield, error));
        if (error)
        {
            break;
        }
    }

    shutdown();
}

void gb28181_tcp_sender_session::shutdown()
{
    const auto self = shared_from_this();
    boost::asio::post(worker_.io(), [self]() { self->safe_shutdown(); });
}

void gb28181_tcp_sender_session::run_write(boost::asio::yield_context yield)
{
    for (;;)
    {
        if (write_queue_.empty())
        {
            return;
        }

        const auto data = write_queue_.front();
        boost::system::error_code error;
        static_cast<void>(transport_->write(*data, yield, error));
        if (error)
        {
            shutdown();
            return;
        }

        queued_write_bytes_ -= data->size();
        write_queue_.pop_front();
    }
}

void gb28181_tcp_sender_session::send_packet(std::vector<std::uint8_t> packet)
{
    if (!transport_)
    {
        return;
    }
    if (packet.size() > std::numeric_limits<std::uint16_t>::max())
    {
        shutdown();
        return;
    }

    const auto frame_bytes = packet.size() + 2U;
    if (frame_bytes > max_write_queue_bytes_ || queued_write_bytes_ > max_write_queue_bytes_ - frame_bytes)
    {
        shutdown();
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
}

}    // namespace media_server
