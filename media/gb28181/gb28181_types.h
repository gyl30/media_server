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

struct gb28181_transport_config
{
    gb28181_transport mode{gb28181_transport::udp};
    boost::asio::ip::address remote_address{};
    std::uint16_t remote_port{};
    std::uint16_t remote_rtp_port{};
    std::uint16_t remote_rtcp_port{};
    std::uint16_t listen_port{};
    std::uint8_t payload_type{};
    std::uint32_t ssrc{};
};

}    // namespace media_server

#endif
