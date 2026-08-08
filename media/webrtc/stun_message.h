#ifndef MEDIA_WEBRTC_STUN_MESSAGE_H
#define MEDIA_WEBRTC_STUN_MESSAGE_H

#include <boost/asio/ip/udp.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace media_server
{

struct stun_binding_request
{
    std::array<std::uint8_t, 12> transaction_id{};
    bool use_candidate{};
};

[[nodiscard]] bool is_stun_message(std::span<const std::uint8_t> packet);

[[nodiscard]] std::optional<stun_binding_request> parse_stun_binding_request(
    std::span<const std::uint8_t> packet,
    std::string_view expected_username,
    std::string_view password);

[[nodiscard]] std::vector<std::uint8_t> make_stun_binding_success_response(
    const stun_binding_request& request,
    const boost::asio::ip::udp::endpoint& remote_endpoint,
    std::string_view password);

}    // namespace media_server

#endif
