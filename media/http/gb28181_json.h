#ifndef MEDIA_HTTP_GB28181_JSON_H
#define MEDIA_HTTP_GB28181_JSON_H

#include <cstdint>
#include <string>
#include <optional>
#include <string_view>

#include <boost/asio/ip/address.hpp>

namespace media_server
{

enum class gb28181_transport
{
    udp,
    tcp_active,
    tcp_passive,
};

struct gb28181_receiver_config
{
    std::string stream_id;
    std::string stream_name;
    gb28181_transport transport{gb28181_transport::udp};
    boost::asio::ip::address remote_address{};
    std::uint16_t remote_port{};
    std::uint16_t listen_port{};
    std::uint8_t payload_type{};
    std::uint32_t ssrc{};
};

struct gb28181_sender_config
{
    std::string stream_id;
    std::string stream_name;
    std::string sender_id;
    gb28181_transport transport{gb28181_transport::udp};
    boost::asio::ip::address remote_address{};
    std::uint16_t remote_port{};
    std::uint16_t remote_rtp_port{};
    std::optional<std::uint16_t> remote_rtcp_port{};
    std::uint16_t listen_port{};
    std::uint8_t payload_type{};
    std::uint32_t ssrc{};
};

struct gb28181_receiver_identity
{
    std::string stream_id;
    std::string stream_name;
};

struct gb28181_sender_identity
{
    std::string stream_id;
    std::string stream_name;
    std::string sender_id;
};

[[nodiscard]] std::optional<gb28181_receiver_config> parse_gb28181_receiver_config(std::string_view body);
[[nodiscard]] std::optional<gb28181_sender_config> parse_gb28181_sender_config(std::string_view body);
[[nodiscard]] std::optional<gb28181_receiver_identity> parse_gb28181_receiver_delete(std::string_view body);
[[nodiscard]] std::optional<gb28181_sender_identity> parse_gb28181_sender_delete(std::string_view body);

}    // namespace media_server

#endif
