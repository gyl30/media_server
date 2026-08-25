#ifndef MEDIA_GB28181_GB28181_TYPES_H
#define MEDIA_GB28181_GB28181_TYPES_H

#include <cstdint>
#include <optional>
#include <string>

#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/udp.hpp>

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

struct gb28181_input_config
{
    std::string stream_name;
    gb28181_description description;
    std::optional<boost::asio::ip::udp::endpoint> remote_rtp_endpoint;
    std::optional<std::uint16_t> remote_rtcp_port;
};

struct gb28181_output_config
{
    std::string stream_name;
    std::string output_id;
    gb28181_description description;
    bool rtcp{};
};

}    // namespace media_server

#endif
