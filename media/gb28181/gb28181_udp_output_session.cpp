#include "media/gb28181/gb28181_udp_output_session.h"

#include "media/gb28181/gb28181_output_media.h"
#include "media/net/udp_socket.h"

#include <boost/asio/post.hpp>

#include <spdlog/spdlog.h>

#include <utility>

namespace media_server
{

gb28181_udp_output_session::gb28181_udp_output_session(boost::asio::any_io_executor executor,
                                                       std::shared_ptr<media_stream> stream,
                                                       gb28181_description description)
    : executor_(std::move(executor)),
      stream_(std::move(stream)),
      stream_name_(stream_ ? stream_->name() : std::string{}),
      description_(std::move(description)),
      remote_endpoint_(description_.address, description_.rtp_port)
{
}

bool gb28181_udp_output_session::startup()
{
    if (closed_ || socket_ || media_ || !stream_ || description_.transport != gb28181_transport::udp || description_.address.is_unspecified())
    {
        return false;
    }

    const auto self = shared_from_this();
    auto socket = std::make_shared<udp_socket>(executor_);
    const auto bind_address = description_.address.is_v4() ? boost::asio::ip::address{boost::asio::ip::address_v4::any()}
                                                           : boost::asio::ip::address{boost::asio::ip::address_v6::any()};
    if (!socket->startup(
            bind_address,
            [self](boost::system::error_code error, std::span<const std::uint8_t>, const boost::asio::ip::udp::endpoint&)
            {
                if (error)
                {
                    self->shutdown();
                }
            },
            [self](boost::system::error_code error, const boost::asio::ip::udp::endpoint&)
            {
                if (error)
                {
                    self->shutdown();
                }
            }))
    {
        return false;
    }
    socket_ = std::move(socket);

    const auto weak = weak_from_this();
    media_ = std::make_shared<gb28181_output_media>(
        executor_,
        stream_,
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
        media_.reset();
        socket_->shutdown();
        socket_.reset();
        return false;
    }

    spdlog::info("gb28181 udp output started stream {} local_port {} remote {}:{}",
                 stream_name_,
                 socket_->local_port(),
                 description_.address.to_string(),
                 description_.rtp_port);
    return true;
}

void gb28181_udp_output_session::shutdown()
{
    const auto self = shared_from_this();
    boost::asio::post(executor_, [self]() { self->safe_shutdown(); });
}

void gb28181_udp_output_session::send_packet(std::vector<std::uint8_t> packet)
{
    if (!closed_ && socket_)
    {
        socket_->send(std::move(packet), remote_endpoint_);
    }
}

void gb28181_udp_output_session::safe_shutdown()
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
    if (socket_)
    {
        socket_->shutdown();
        socket_.reset();
    }
    stream_.reset();
    spdlog::debug("gb28181 udp output shutdown {}", stream_name_);
}

}    // namespace media_server
