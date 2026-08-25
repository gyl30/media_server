#ifndef MEDIA_GB28181_GB28181_TYPES_H
#define MEDIA_GB28181_GB28181_TYPES_H

#include <cstdint>

#include <boost/asio/ip/address.hpp>

namespace media_server
{

enum class gb28181_transport
{
    udp,
    tcp_active,
    tcp_passive,
};

struct gb28181_description
{
    gb28181_transport transport{gb28181_transport::udp};
    boost::asio::ip::address address;
    std::uint16_t rtp_port{};
    std::uint16_t rtcp_port{};
    std::uint8_t payload_type{};
    std::uint32_t ssrc{};
};

}    // namespace media_server

#endif
