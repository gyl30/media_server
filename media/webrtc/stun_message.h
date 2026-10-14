#ifndef MEDIA_WEBRTC_STUN_MESSAGE_H
#define MEDIA_WEBRTC_STUN_MESSAGE_H

#include <span>
#include <array>
#include <vector>
#include <cstdint>
#include <optional>
#include <string_view>

#include <boost/asio/ip/udp.hpp>

namespace media_server
{

struct stun_binding_request
{
    std::array<std::uint8_t, 12> transaction_id{};
    std::vector<std::uint16_t> unknown_required_attributes;
    bool priority{};
    bool use_candidate{};
    bool ice_controlling{};
    bool ice_controlled{};
};

[[nodiscard]] bool is_stun_message(std::span<const std::uint8_t> packet);

[[nodiscard]] std::optional<stun_binding_request> parse_stun_binding_request(std::span<const std::uint8_t> packet,
                                                                             std::string_view expected_username,
                                                                             std::string_view password);

[[nodiscard]] std::vector<std::uint8_t> make_stun_binding_success_response(const stun_binding_request& request,
                                                                           const boost::asio::ip::udp::endpoint& remote_endpoint,
                                                                           std::string_view password);

// ICE controlling 方的连通性检查（含 USE-CANDIDATE，按积极提名处理），username 为 "对端ufrag:本端ufrag"，password 为对端密码。
[[nodiscard]] std::vector<std::uint8_t> make_stun_binding_request(const std::array<std::uint8_t, 12>& transaction_id,
                                                                  std::string_view username,
                                                                  std::string_view password,
                                                                  std::uint32_t priority,
                                                                  std::uint64_t tie_breaker);

// 校验 Binding 成功响应的 MESSAGE-INTEGRITY（对端密码）与 FINGERPRINT，返回其事务 ID。
[[nodiscard]] std::optional<std::array<std::uint8_t, 12>> parse_stun_binding_success_response(std::span<const std::uint8_t> packet,
                                                                                             std::string_view password);

[[nodiscard]] std::vector<std::uint8_t> make_stun_binding_error_response(const stun_binding_request& request,
                                                                         std::uint16_t error_code,
                                                                         std::string_view reason,
                                                                         std::span<const std::uint16_t> unknown_attributes,
                                                                         std::string_view password);

}    // namespace media_server

#endif
