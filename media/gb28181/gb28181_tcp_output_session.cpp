#include <limits>
#include <utility>
#include <algorithm>

#include <spdlog/spdlog.h>
#include <boost/asio/post.hpp>

#include "media/net/tcp_connection.h"
#include "media/gb28181/gb28181_output_media.h"
#include "media/gb28181/gb28181_tcp_output_session.h"

namespace media_server
{

gb28181_tcp_output_session::gb28181_tcp_output_session(boost::asio::ip::tcp::socket socket,
                                                       std::shared_ptr<media_stream> stream,
                                                       std::uint8_t payload_type,
                                                       std::uint32_t ssrc)
    : executor_(socket.get_executor()),
      stream_(std::move(stream)),
      stream_name_(stream_ ? stream_->name() : std::string{}),
      payload_type_(payload_type),
      ssrc_(ssrc),
      connection_(std::make_shared<tcp_connection>(std::move(socket)))
{
}

bool gb28181_tcp_output_session::startup()
{
    if (closed_ || !connection_ || media_ || !stream_)
    {
        return false;
    }

    const auto self = shared_from_this();
    connection_->startup(
        [self](boost::system::error_code error, std::span<const std::uint8_t>)
        {
            if (error)
            {
                self->shutdown();
            }
        },
        [self](boost::system::error_code error, std::size_t)
        {
            if (error)
            {
                self->shutdown();
            }
        });

    const auto weak = weak_from_this();
    media_ = std::make_shared<gb28181_output_media>(
        executor_,
        stream_,
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
        media_.reset();
        connection_->shutdown();
        connection_.reset();
        return false;
    }

    spdlog::info("gb28181 tcp output started stream {}", stream_name_);
    return true;
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
    stream_.reset();
    spdlog::debug("gb28181 tcp output shutdown {}", stream_name_);
}

}    // namespace media_server
