#include <mutex>
#include <climits>
#include <cstring>
#include <utility>
#include <string_view>

#include <srtp2/srtp.h>
#include <spdlog/spdlog.h>

#include "media/webrtc/srtp_transport.h"

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

    if (profile == "SRTP_AEAD_AES_256_GCM")
    {
        srtp_crypto_policy_set_aes_gcm_256_16_auth(&policy.rtp);
        srtp_crypto_policy_set_aes_gcm_256_16_auth(&policy.rtcp);
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

bool create_session(std::string_view profile, std::vector<std::uint8_t>& master_key, srtp_t& session)
{
    srtp_policy_t policy{};
    if (!set_crypto_policy(profile, policy) || master_key.empty())
    {
        return false;
    }

    policy.ssrc.type = ssrc_any_outbound;
    policy.ssrc.value = 0;
    policy.key = master_key.data();
    policy.window_size = 1024;
    policy.next = nullptr;
    return srtp_create(&session, &policy) == srtp_err_status_ok;
}

}    // namespace

struct srtp_transport::context
{
    srtp_t outbound{};
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
    auto master_key = make_master_key(keying_material.server_write_key, keying_material.server_write_salt);
    if (!create_session(keying_material.profile, master_key, state->outbound))
    {
        spdlog::debug("webrtc srtp outbound context create failed profile {}", keying_material.profile);
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

bool srtp_transport::is_rtp_or_rtcp(std::span<const std::uint8_t> packet) noexcept { return packet.size() >= 2U && (packet[0] & 0xC0U) == 0x80U; }

}    // namespace media_server
