#ifndef MEDIA_GB28181_GB28181_TYPES_H
#define MEDIA_GB28181_GB28181_TYPES_H

#include <cstdint>
#include <string>

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

struct gb28181_receiver_config
{
    std::string stream_name;
    gb28181_transport_config transport;
};

struct gb28181_sender_config
{
    std::string stream_name;
    std::string sender_id;
    gb28181_transport_config transport;
    bool rtcp_enabled{};
};

}    // namespace media_server

#endif
