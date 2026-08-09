#ifndef MEDIA_WEBRTC_DTLS_TRANSPORT_H
#define MEDIA_WEBRTC_DTLS_TRANSPORT_H

#include "media/webrtc/dtls_certificate.h"

#include <openssl/ssl.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace media_server
{

struct dtls_srtp_keying_material
{
    std::string profile;
    std::vector<std::uint8_t> client_write_key;
    std::vector<std::uint8_t> client_write_salt;
    std::vector<std::uint8_t> server_write_key;
    std::vector<std::uint8_t> server_write_salt;
};

class dtls_transport final
{
   public:
    using send_callback = std::function<void(std::span<const std::uint8_t>)>;

    dtls_transport(
        std::shared_ptr<dtls_certificate> certificate,
        std::string remote_fingerprint,
        send_callback send);

    bool start();
    bool handle_datagram(std::span<const std::uint8_t> packet);
    bool handle_timeout();

    [[nodiscard]] bool connected() const noexcept;
    [[nodiscard]] std::optional<std::chrono::milliseconds> timeout() const;
    [[nodiscard]] const std::optional<dtls_srtp_keying_material>& srtp_keying_material() const noexcept;
    [[nodiscard]] static bool is_dtls_packet(std::span<const std::uint8_t> packet) noexcept;

   private:
    struct ssl_context_deleter
    {
        void operator()(SSL_CTX* value) const noexcept;
    };

    struct ssl_deleter
    {
        void operator()(SSL* value) const noexcept;
    };

    using ssl_context_ptr = std::unique_ptr<SSL_CTX, ssl_context_deleter>;
    using ssl_ptr = std::unique_ptr<SSL, ssl_deleter>;

    void close();
    bool finish_handshake();
    bool verify_peer_fingerprint() const;
    std::optional<dtls_srtp_keying_material> export_srtp_keying_material() const;
    bool pump_outgoing();

    std::shared_ptr<dtls_certificate> certificate_;
    std::string remote_fingerprint_;
    send_callback send_;
    ssl_context_ptr context_;
    ssl_ptr ssl_;
    BIO* read_bio_{};
    BIO* write_bio_{};
    std::optional<dtls_srtp_keying_material> srtp_keying_material_;
};

}    // namespace media_server

#endif
