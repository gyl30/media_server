#ifndef MEDIA_GB28181_GB28181_SDP_H
#define MEDIA_GB28181_GB28181_SDP_H

#include <boost/asio/ip/address.hpp>

#include <cstdint>
#include <optional>
#include <string_view>

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

[[nodiscard]] std::optional<gb28181_description> parse_gb28181_sdp(std::string_view text);

}    // namespace media_server

#endif
