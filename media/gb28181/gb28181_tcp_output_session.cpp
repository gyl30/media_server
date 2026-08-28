#include <limits>
#include <utility>
#include <algorithm>

#include <spdlog/spdlog.h>
#include <boost/asio/post.hpp>

#include "media/net/tcp_connection.h"
#include "media/core/stream_registry.h"
#include "media/gb28181/gb28181_output_media.h"
#include "media/gb28181/gb28181_tcp_output_session.h"

namespace media_server
{

gb28181_tcp_output_session::gb28181_tcp_output_session(boost::asio::any_io_executor executor,
                                                       std::shared_ptr<tcp_socket_source> socket_source,
                                                       std::weak_ptr<media_stream> stream,
                                                       std::string stream_name,
                                                       std::string output_id,
                                                       std::uint8_t payload_type,
                                                       std::uint32_t ssrc)
    : executor_(std::move(executor)),
      socket_source_(std::move(socket_source)),
      stream_(std::move(stream)),
      stream_name_(std::move(stream_name)),
      output_id_(std::move(output_id)),
      payload_type_(payload_type),
      ssrc_(ssrc)
{
}

bool gb28181_tcp_output_session::startup()
{
    if (closed_ || !socket_source_)
    {
        return false;
    }

    const auto weak = weak_from_this();
    boost::system::error_code error;
    socket_source_->startup(
        [weak](boost::system::error_code source_error, boost::asio::ip::tcp::socket socket) mutable
        {
            if (const auto self = weak.lock())
            {
                self->on_socket_result(source_error, std::move(socket));
                return;
            }
            boost::system::error_code ignored;
            socket.close(ignored);
        },
        error);
    if (error)
    {
        spdlog::error("gb28181 tcp output source startup failed stream {} output {} error {}", stream_name_, output_id_, error.message());
        shutdown();
        return false;
    }
    return true;
}

void gb28181_tcp_output_session::on_socket_result(boost::system::error_code error, boost::asio::ip::tcp::socket socket)
{
    if (error)
    {
        spdlog::warn("gb28181 tcp output source failed stream {} output {} error {}", stream_name_, output_id_, error.message());
        shutdown();
        return;
    }
    if (closed_)
    {
        boost::system::error_code ignored;
        socket.close(ignored);
        return;
    }

    const auto stream = stream_.lock();
    if (!stream)
    {
        boost::system::error_code ignored;
        socket.close(ignored);
        shutdown();
        return;
    }

    socket_source_.reset();
    connection_ = std::make_shared<tcp_connection>(std::move(socket));
    const auto weak = weak_from_this();
    connection_->startup(
        [weak](boost::system::error_code connection_error, std::span<const std::uint8_t>)
        {
            if (connection_error)
            {
                if (const auto self = weak.lock())
                {
                    self->shutdown();
                }
            }
        },
        [weak](boost::system::error_code connection_error, std::size_t)
        {
            if (connection_error)
            {
                if (const auto self = weak.lock())
                {
                    self->shutdown();
                }
            }
        });

    media_ = std::make_shared<gb28181_output_media>(
        executor_,
        stream,
        payload_type_,
        ssrc_,
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
        media_->shutdown();
        media_.reset();
        connection_->shutdown();
        connection_.reset();
        shutdown();
        return;
    }

    spdlog::info("gb28181 tcp output started stream {} output {}", stream_name_, output_id_);
}

void gb28181_tcp_output_session::shutdown()
{
    const auto self = shared_from_this();
    boost::asio::post(executor_, [self]() { self->safe_shutdown(); });
}

void gb28181_tcp_output_session::send_packet(std::vector<std::uint8_t> packet)
{
    if (closed_ || !connection_)
    {
        return;
    }
    if (packet.size() > std::numeric_limits<std::uint16_t>::max())
    {
        shutdown();
        return;
    }

    std::vector<std::uint8_t> frame(packet.size() + 2U);
    const auto length = static_cast<std::uint16_t>(packet.size());
    frame[0] = static_cast<std::uint8_t>(length >> 8U);
    frame[1] = static_cast<std::uint8_t>(length & 0xffU);
    std::copy(packet.begin(), packet.end(), frame.begin() + 2);
    connection_->write(frame);
}

void gb28181_tcp_output_session::safe_shutdown()
{
    if (closed_)
    {
        return;
    }
    closed_ = true;
    registry::instance().remove_output_session(stream_name_, output_id_, *this);
    if (socket_source_)
    {
        socket_source_->shutdown();
        socket_source_.reset();
    }
    if (media_)
    {
        media_->shutdown();
        media_.reset();
    }
    if (connection_)
    {
        connection_->shutdown();
        connection_.reset();
    }
    spdlog::debug("gb28181 tcp output shutdown {} output {}", stream_name_, output_id_);
}

}    // namespace media_server
