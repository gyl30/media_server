#ifndef MEDIA_WEBRTC_SRTP_TRANSPORT_H
#define MEDIA_WEBRTC_SRTP_TRANSPORT_H

#include <span>
#include <memory>
#include <vector>
#include <cstdint>
#include <optional>

#include "media/webrtc/dtls_transport.h"

namespace media_server
{

class srtp_transport final
{
   public:
    srtp_transport();
    ~srtp_transport();

    bool startup(const dtls_srtp_keying_material& keying_material);

    [[nodiscard]] std::optional<std::vector<std::uint8_t>> protect_rtp(std::span<const std::uint8_t> packet);
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> protect_rtcp(std::span<const std::uint8_t> packet);
    [[nodiscard]] static bool is_rtp_or_rtcp(std::span<const std::uint8_t> packet) noexcept;

   private:
    void shutdown();

    struct context;
    std::unique_ptr<context> context_;
};

}    // namespace media_server

#endif
