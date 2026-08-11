#include "media/webrtc/srtp_transport.h"

#include <srtp2/srtp.h>

#include <spdlog/spdlog.h>

#include <climits>
#include <cstring>
#include <mutex>
#include <string_view>
#include <utility>

namespace media_server
{
namespace
{

bool initialize_srtp()
{
    static std::once_flag flag;
    static bool initialized = false;
    std::call_once(flag, []() { initialized = srtp_init() == srtp_err_status_ok; });
    return initialized;
}

bool set_crypto_policy(std::string_view profile, srtp_policy_t& policy)
{
    if (profile == "SRTP_AEAD_AES_128_GCM")
    {
        srtp_crypto_policy_set_aes_gcm_128_16_auth(&policy.rtp);
        srtp_crypto_policy_set_aes_gcm_128_16_auth(&policy.rtcp);
        return true;
    }

    if (profile == "SRTP_AES128_CM_SHA1_80")
    {
        srtp_crypto_policy_set_rtp_default(&policy.rtp);
        srtp_crypto_policy_set_rtcp_default(&policy.rtcp);
        return true;
    }

    return false;
}

std::vector<std::uint8_t> make_master_key(const std::vector<std::uint8_t>& key, const std::vector<std::uint8_t>& salt)
{
    std::vector<std::uint8_t> result;
    result.reserve(key.size() + salt.size());
    result.insert(result.end(), key.begin(), key.end());
    result.insert(result.end(), salt.begin(), salt.end());
    return result;
}

bool create_session(std::string_view profile, srtp_ssrc_type_t direction, std::vector<std::uint8_t>& master_key, srtp_t& session)
{
    srtp_policy_t policy{};
    if (!set_crypto_policy(profile, policy) || master_key.empty())
    {
        return false;
    }

    policy.ssrc.type = direction;
    policy.ssrc.value = 0;
    policy.key = master_key.data();
    policy.window_size = 1024;
    policy.allow_repeat_tx = direction == ssrc_any_outbound ? 1 : 0;
    policy.next = nullptr;
    return srtp_create(&session, &policy) == srtp_err_status_ok;
}

bool is_rtcp(std::span<const std::uint8_t> packet) noexcept
{
    if (packet.size() < 2U)
    {
        return false;
    }

    const auto packet_type = packet[1];
    return packet_type >= 192U && packet_type <= 223U;
}

}    // namespace

struct srtp_transport::context
{
    srtp_t outbound{};
    srtp_t inbound{};
    std::vector<std::uint8_t> outbound_key;
    std::vector<std::uint8_t> inbound_key;
};

srtp_transport::srtp_transport() = default;

srtp_transport::~srtp_transport() { shutdown(); }

bool srtp_transport::startup(const dtls_srtp_keying_material& keying_material)
{
    if (context_ || !initialize_srtp())
    {
        spdlog::debug("webrtc srtp startup rejected or library init failed");
        return false;
    }

    spdlog::debug("webrtc srtp transport startup profile {}", keying_material.profile);

    auto state = std::make_unique<struct context>();
    state->outbound_key = make_master_key(keying_material.server_write_key, keying_material.server_write_salt);
    state->inbound_key = make_master_key(keying_material.client_write_key, keying_material.client_write_salt);

    if (!create_session(keying_material.profile, ssrc_any_outbound, state->outbound_key, state->outbound))
    {
        spdlog::debug("webrtc srtp outbound context create failed profile {}", keying_material.profile);
        return false;
    }

    if (!create_session(keying_material.profile, ssrc_any_inbound, state->inbound_key, state->inbound))
    {
        spdlog::debug("webrtc srtp inbound context create failed profile {}", keying_material.profile);
        srtp_dealloc(state->outbound);
        state->outbound = nullptr;
        return false;
    }

    context_ = std::move(state);
    spdlog::debug("webrtc srtp transport started profile {}", keying_material.profile);
    return true;
}

void srtp_transport::shutdown()
{
    if (!context_)
    {
        return;
    }

    if (context_->outbound != nullptr)
    {
        srtp_dealloc(context_->outbound);
    }
    if (context_->inbound != nullptr)
    {
        srtp_dealloc(context_->inbound);
    }
    context_.reset();
}

std::optional<std::vector<std::uint8_t>> srtp_transport::protect_rtp(std::span<const std::uint8_t> packet)
{
    if (!context_ || packet.empty() || packet.size() > static_cast<std::size_t>(INT_MAX))
    {
        return std::nullopt;
    }

    std::uint32_t trailer_size = 0;
    if (srtp_get_protect_trailer_length(context_->outbound, 0, 0, &trailer_size) != srtp_err_status_ok)
    {
        return std::nullopt;
    }

    std::vector<std::uint8_t> output(packet.size() + trailer_size);
    std::memcpy(output.data(), packet.data(), packet.size());
    int size = static_cast<int>(packet.size());
    const auto status = srtp_protect(context_->outbound, output.data(), &size);
    if (status != srtp_err_status_ok || size < 0)
    {
        spdlog::debug("webrtc srtp protect failed status {}", static_cast<int>(status));
        return std::nullopt;
    }
    output.resize(static_cast<std::size_t>(size));
    return output;
}

std::optional<std::vector<std::uint8_t>> srtp_transport::protect_rtcp(std::span<const std::uint8_t> packet)
{
    if (!context_ || packet.empty() || packet.size() > static_cast<std::size_t>(INT_MAX))
    {
        return std::nullopt;
    }

    std::uint32_t trailer_size = 0;
    if (srtp_get_protect_rtcp_trailer_length(context_->outbound, 0, 0, &trailer_size) != srtp_err_status_ok)
    {
        return std::nullopt;
    }

    std::vector<std::uint8_t> output(packet.size() + trailer_size);
    std::memcpy(output.data(), packet.data(), packet.size());
    int size = static_cast<int>(packet.size());
    const auto status = srtp_protect_rtcp(context_->outbound, output.data(), &size);
    if (status != srtp_err_status_ok || size < 0)
    {
        spdlog::debug("webrtc srtcp protect failed status {}", static_cast<int>(status));
        return std::nullopt;
    }
    output.resize(static_cast<std::size_t>(size));
    return output;
}

std::optional<srtp_packet> srtp_transport::unprotect(std::span<const std::uint8_t> packet)
{
    if (!context_ || packet.empty() || packet.size() > static_cast<std::size_t>(INT_MAX))
    {
        return std::nullopt;
    }

    const bool packet_is_rtcp = is_rtcp(packet);
    std::vector<std::uint8_t> output(packet.begin(), packet.end());
    int size = static_cast<int>(output.size());
    const auto status =
        packet_is_rtcp ? srtp_unprotect_rtcp(context_->inbound, output.data(), &size) : srtp_unprotect(context_->inbound, output.data(), &size);
    if (status != srtp_err_status_ok || size < 0)
    {
        spdlog::debug("webrtc srtp unprotect failed rtcp {} status {}", packet_is_rtcp, static_cast<int>(status));
        return std::nullopt;
    }

    output.resize(static_cast<std::size_t>(size));
    return srtp_packet{.rtcp = packet_is_rtcp, .bytes = std::move(output)};
}

bool srtp_transport::is_rtp_or_rtcp(std::span<const std::uint8_t> packet) noexcept { return packet.size() >= 2U && (packet[0] & 0xC0U) == 0x80U; }

}    // namespace media_server
