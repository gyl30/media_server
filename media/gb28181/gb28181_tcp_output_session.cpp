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
#include "media/gb28181/gb28181_output_media.h"
#include "media/net/worker_context.h"
#include "media/gb28181/gb28181_tcp_output_session.h"

namespace media_server
{
namespace
{
constexpr auto slow_write_timeout = std::chrono::seconds(15);
}

gb28181_tcp_output_session::gb28181_tcp_output_session(worker_context& worker,
                                                       std::weak_ptr<media_stream> stream,
                                                       std::string stream_name,
                                                       std::string output_id,
                                                       gb28181_description description,
                                                       std::chrono::milliseconds establishment_timeout)
    : worker_(worker),
      stream_(std::move(stream)),
      stream_name_(std::move(stream_name)),
      output_id_(std::move(output_id)),
      description_(std::move(description)),
      establishment_timeout_(establishment_timeout),
      socket_(worker_.io())
{
}

bool gb28181_tcp_output_session::startup()
{
    if (description_.transport == gb28181_transport::tcp_passive)
    {
        listener_ = std::make_unique<tcp_listener>(worker_.io(), description_.rtp_port, description_.address);
        boost::system::error_code error;
        listener_->startup(error);
        if (error)
        {
            spdlog::error("gb28181 tcp output listener startup failed stream {} output {} error {}",
                          stream_name_,
                          output_id_,
                          error.message());
            listener_.reset();
            return false;
        }
    }

    const auto self = shared_from_this();
    boost::asio::spawn(worker_.io(), [self](boost::asio::yield_context yield) { self->run(yield); }, boost::asio::detached);
    return true;
}

void gb28181_tcp_output_session::run(boost::asio::yield_context yield)
{
    boost::system::error_code error;
    if (description_.transport == gb28181_transport::tcp_passive)
    {
        listener_->accept(socket_, establishment_timeout_, yield, error);
        listener_->shutdown();
        listener_.reset();
    }
    else
    {
        socket_.async_connect(boost::asio::ip::tcp::endpoint{description_.address, description_.rtp_port},
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
            spdlog::warn("gb28181 tcp output establishment failed stream {} output {} error {}", stream_name_, output_id_, error.message());
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
    media_ = std::make_shared<gb28181_output_media>(
        worker_,
        stream,
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
        shutdown();
        return;
    }

    spdlog::info("gb28181 tcp output started stream {} output {}", stream_name_, output_id_);

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

void gb28181_tcp_output_session::shutdown()
{
    const auto self = shared_from_this();
    boost::asio::post(worker_.io(), [self]() { self->safe_shutdown(); });
}

void gb28181_tcp_output_session::run_write(boost::asio::yield_context yield)
{
    for (;;)
    {
        if (write_queue_.empty())
        {
            return;
        }

        const auto data = write_queue_.front();
        boost::system::error_code error;
        const auto started_at = std::chrono::steady_clock::now();
        static_cast<void>(transport_->write(*data, yield, error));
        if (error)
        {
            shutdown();
            return;
        }

        write_queue_.pop_front();
        if (std::chrono::steady_clock::now() - started_at > slow_write_timeout)
        {
            shutdown();
            return;
        }
    }
}

void gb28181_tcp_output_session::send_packet(std::vector<std::uint8_t> packet)
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

    auto frame = std::make_shared<std::vector<std::uint8_t>>(packet.size() + 2U);
    const auto length = static_cast<std::uint16_t>(packet.size());
    (*frame)[0] = static_cast<std::uint8_t>(length >> 8U);
    (*frame)[1] = static_cast<std::uint8_t>(length & 0xffU);
    std::copy(packet.begin(), packet.end(), frame->begin() + 2);

    const bool start_write = write_queue_.empty();
    write_queue_.push_back(std::move(frame));
    if (start_write)
    {
        const auto self = shared_from_this();
        boost::asio::spawn(worker_.io(), [self](boost::asio::yield_context write_yield) { self->run_write(write_yield); }, boost::asio::detached);
    }
}

void gb28181_tcp_output_session::safe_shutdown()
{
    if (closed_)
    {
        return;
    }
    closed_ = true;
    registry::instance().remove_output_session(stream_name_, output_id_, *this);
    if (listener_)
    {
        listener_->shutdown();
    }
    boost::system::error_code error;
    socket_.cancel(error);
    socket_.close(error);
    if (media_)
    {
        media_->shutdown();
        media_.reset();
    }
    if (transport_)
    {
        transport_->shutdown();
    }
    spdlog::debug("gb28181 tcp output shutdown {} output {}", stream_name_, output_id_);
}

}    // namespace media_server
