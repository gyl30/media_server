#include <array>
#include <chrono>
#include <vector>
#include <utility>
#include <unistd.h>

#include <spdlog/spdlog.h>
#include <boost/asio/read.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/http.hpp>
#include <boost/asio/executor_work_guard.hpp>

#include "media/core/scope_exit.h"
#include "media/net/media_port_pool.h"
#include "media/gb28181/gb28181_playback_session.h"

extern "C"
{
#include "rtp-packet.h"
}

namespace media_server
{
namespace
{
using tcp = boost::asio::ip::tcp;
namespace http = boost::beast::http;
constexpr std::chrono::seconds write_timeout{20};
}    // namespace

gb28181_playback_session::gb28181_playback_session(worker_context& worker, std::string stream_id, std::uint8_t payload_type, std::uint32_t ssrc)
    : worker_(worker), stream_id_(std::move(stream_id)), payload_type_(payload_type), ssrc_(ssrc), listener_(worker.io()), input_(worker.io())
{
}

std::optional<std::uint16_t> gb28181_playback_session::startup(boost::asio::ip::address bind_address)
{
    if (worker_.stop_requested())
    {
        return std::nullopt;
    }
    local_port_ = media_port_pool::instance().acquire();
    if (!local_port_)
    {
        return std::nullopt;
    }
    const tcp::endpoint endpoint{std::move(bind_address), *local_port_};
    boost::system::error_code error;
    listener_.open(endpoint.protocol(), error);
    if (!error)
    {
        listener_.set_option(tcp::acceptor::reuse_address(true), error);
    }
    if (!error)
    {
        listener_.bind(endpoint, error);
    }
    if (!error)
    {
        listener_.listen(1, error);
    }
    if (error)
    {
        return std::nullopt;
    }
    const auto self = shared_from_this();
    shutdown_subscription_ = worker_.subscribe_shutdown([self]() { self->shutdown(); });
    return shutdown_subscription_ ? local_port_ : std::nullopt;
}

void gb28181_playback_session::attach(tcp::socket socket, unsigned http_version)
{
    auto work = boost::asio::make_work_guard(worker_.io());
    if (worker_.stop_requested())
    {
        return;
    }
    const auto self = shared_from_this();
    boost::asio::post(worker_.io(),
                      [self, socket = std::move(socket), http_version, work = std::move(work)]() mutable
                      {
                          if (self->worker_.stop_requested())
                          {
                              return;
                          }
                          boost::system::error_code error;
                          const auto endpoint = socket.local_endpoint(error);
                          if (error)
                          {
                              return;
                          }
                          tcp::socket rebound(self->worker_.io());
                          const auto native = socket.release(error);
                          if (error)
                          {
                              return;
                          }
                          scope_exit close_native([native]() { ::close(native); });
                          rebound.assign(endpoint.protocol(), native, error);
                          if (error)
                          {
                              return;
                          }
                          close_native.release();
                          self->worker_.spawn([self, socket = std::move(rebound), http_version](boost::asio::yield_context yield) mutable
                                              { self->run(std::move(socket), http_version, yield); });
                      });
}

void gb28181_playback_session::update_ssrc(std::uint32_t ssrc)
{
    const auto self = shared_from_this();
    boost::asio::post(worker_.io(), [self, ssrc]() { self->ssrc_ = ssrc; });
}

void gb28181_playback_session::run(tcp::socket socket, unsigned http_version, boost::asio::yield_context yield)
{
    boost::system::error_code error;
    if (!listener_.is_open() || consumer_ || session_registry::instance().find_receiver_session(stream_id_).get() != this)
    {
        boost::beast::tcp_stream rejected(std::move(socket));
        rejected.expires_after(write_timeout);
        http::response<http::empty_body> response(http::status::conflict, http_version);
        response.keep_alive(false);
        response.prepare_payload();
        http::async_write(rejected, response, yield[error]);
        return;
    }
    consumer_ = std::make_unique<boost::beast::tcp_stream>(std::move(socket));
    consumer_->expires_after(write_timeout);
    http::response<http::empty_body> response(http::status::ok, http_version);
    response.set(http::field::server, "media_server");
    response.set(http::field::content_type, "video/mpeg");
    response.keep_alive(false);
    response.chunked(true);
    http::response_serializer<http::empty_body> serializer(response);
    http::async_write_header(*consumer_, serializer, yield[error]);
    if (error || yield.cancelled() != boost::asio::cancellation_type::none)
    {
        shutdown();
        return;
    }
    consumer_->expires_never();
    const auto self = shared_from_this();
    worker_.spawn([self](boost::asio::yield_context read_yield) { self->watch_consumer(read_yield); });
    listener_.async_accept(input_, yield[error]);
    if (error || yield.cancelled() != boost::asio::cancellation_type::none)
    {
        shutdown();
        return;
    }
    listener_.close(error);
    std::vector<std::uint8_t> datagram;
    std::optional<std::uint16_t> next_sequence;
    for (;;)
    {
        std::array<std::uint8_t, 2> length{};
        const auto count = boost::asio::async_read(input_, boost::asio::buffer(length), yield[error]);
        if (error || yield.cancelled() != boost::asio::cancellation_type::none)
        {
            if (error == boost::asio::error::eof && count == 0 && next_sequence && yield.cancelled() == boost::asio::cancellation_type::none)
            {
                boost::asio::async_write(*consumer_, http::make_chunk_last(), yield[error]);
            }
            shutdown();
            return;
        }
        const auto size = (static_cast<std::size_t>(length[0]) << 8U) | length[1];
        if (size == 0)
        {
            continue;
        }
        datagram.resize(size);
        boost::asio::async_read(input_, boost::asio::buffer(datagram), yield[error]);
        if (error || yield.cancelled() != boost::asio::cancellation_type::none)
        {
            shutdown();
            return;
        }
        rtp_packet_t packet{};
        if (rtp_packet_deserialize(&packet, datagram.data(), static_cast<int>(datagram.size())) != 0 || packet.payloadlen <= 0 ||
            packet.rtp.pt != payload_type_ || packet.rtp.ssrc != ssrc_ || (next_sequence && packet.rtp.seq != *next_sequence))
        {
            spdlog::warn("gb28181 playback RTP invalid stream {}", stream_id_);
            shutdown();
            return;
        }
        next_sequence = static_cast<std::uint16_t>(packet.rtp.seq + 1U);
        boost::asio::async_write(
            *consumer_, http::make_chunk(boost::asio::buffer(packet.payload, static_cast<std::size_t>(packet.payloadlen))), yield[error]);
        if (error || yield.cancelled() != boost::asio::cancellation_type::none)
        {
            shutdown();
            return;
        }
    }
}

void gb28181_playback_session::watch_consumer(boost::asio::yield_context yield)
{
    std::array<std::uint8_t, 1> buffer{};
    boost::system::error_code error;
    consumer_->socket().async_read_some(boost::asio::buffer(buffer), yield[error]);
    shutdown();
}

void gb28181_playback_session::shutdown()
{
    const auto self = shared_from_this();
    boost::asio::post(worker_.io(), [self]() { self->safe_shutdown(); });
}

void gb28181_playback_session::safe_shutdown()
{
    session_registry::instance().remove_receiver_session(stream_id_, *this);
    shutdown_subscription_.reset();
    boost::system::error_code error;
    listener_.close(error);
    input_.close(error);
    if (consumer_)
    {
        consumer_->socket().shutdown(tcp::socket::shutdown_both, error);
        consumer_->socket().close(error);
    }
    if (local_port_)
    {
        media_port_pool::instance().release(*local_port_);
        local_port_.reset();
    }
}

}    // namespace media_server
