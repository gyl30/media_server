#include "media/webrtc/dtls_transport.h"

#include <openssl/err.h>
#include <openssl/srtp.h>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <climits>
#include <string_view>
#include <utility>

namespace media_server
{
namespace
{

constexpr std::string_view dtls_srtp_exporter_label = "EXTRACTOR-dtls_srtp";

int accept_peer_certificate(int, X509_STORE_CTX*)
{
    return 1;
}

std::string lower_copy(std::string_view value)
{
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return result;
}

std::optional<std::vector<std::uint8_t>> parse_sha256_fingerprint(std::string_view value)
{
    const auto space = value.find(' ');
    if (space == std::string_view::npos || lower_copy(value.substr(0, space)) != "sha-256")
    {
        return std::nullopt;
    }

    const auto digest_text = value.substr(space + 1U);
    std::vector<std::uint8_t> digest;
    digest.reserve(32);

    std::uint8_t byte = 0;
    bool high_nibble = true;
    std::size_t hex_count = 0;
    for (const char character : digest_text)
    {
        if (character == ':')
        {
            continue;
        }

        std::uint8_t nibble = 0;
        if (character >= '0' && character <= '9')
        {
            nibble = static_cast<std::uint8_t>(character - '0');
        }
        else if (character >= 'a' && character <= 'f')
        {
            nibble = static_cast<std::uint8_t>(character - 'a' + 10);
        }
        else if (character >= 'A' && character <= 'F')
        {
            nibble = static_cast<std::uint8_t>(character - 'A' + 10);
        }
        else
        {
            return std::nullopt;
        }

        if (high_nibble)
        {
            byte = static_cast<std::uint8_t>(nibble << 4U);
        }
        else
        {
            byte = static_cast<std::uint8_t>(byte | nibble);
            digest.push_back(byte);
        }
        high_nibble = !high_nibble;
        ++hex_count;
    }

    if (!high_nibble || hex_count != 64U || digest.size() != 32U)
    {
        return std::nullopt;
    }
    return digest;
}

struct srtp_profile_size
{
    std::size_t key_size{};
    std::size_t salt_size{};
};

std::optional<srtp_profile_size> profile_size(std::string_view profile)
{
    if (profile == "SRTP_AES128_CM_SHA1_80" || profile == "SRTP_AES128_CM_SHA1_32")
    {
        return srtp_profile_size{.key_size = 16, .salt_size = 14};
    }
    if (profile == "SRTP_AEAD_AES_128_GCM")
    {
        return srtp_profile_size{.key_size = 16, .salt_size = 12};
    }
    return std::nullopt;
}

}    // namespace

dtls_transport::dtls_transport(
    std::shared_ptr<dtls_certificate> certificate,
    std::string remote_fingerprint,
    send_callback send)
    : certificate_(std::move(certificate)),
      remote_fingerprint_(std::move(remote_fingerprint)),
      send_(std::move(send))
{
}

bool dtls_transport::start()
{
    if (ssl_ || !certificate_ || !send_ || !valid_sha256_fingerprint(remote_fingerprint_))
    {
        spdlog::debug("webrtc dtls start rejected invalid state or fingerprint");
        return false;
    }

    context_.reset(SSL_CTX_new(DTLS_method()));
    if (!context_ ||
        SSL_CTX_set_min_proto_version(context_.get(), DTLS1_2_VERSION) != 1 ||
        SSL_CTX_set_max_proto_version(context_.get(), DTLS1_2_VERSION) != 1 ||
        SSL_CTX_set_cipher_list(context_.get(), "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384") != 1 ||
        SSL_CTX_set_tlsext_use_srtp(context_.get(), "SRTP_AEAD_AES_128_GCM:SRTP_AES128_CM_SHA1_80") != 0 ||
        SSL_CTX_use_certificate(context_.get(), certificate_->certificate()) != 1 ||
        SSL_CTX_use_PrivateKey(context_.get(), certificate_->private_key()) != 1 ||
        SSL_CTX_check_private_key(context_.get()) != 1)
    {
        spdlog::debug("webrtc dtls context configure failed");
        close();
        return false;
    }

    SSL_CTX_set_read_ahead(context_.get(), 1);
    SSL_CTX_set_session_cache_mode(context_.get(), SSL_SESS_CACHE_OFF);
    SSL_CTX_set_verify(context_.get(), SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, accept_peer_certificate);

    ssl_.reset(SSL_new(context_.get()));
    if (!ssl_)
    {
        close();
        return false;
    }

    SSL_set_mtu(ssl_.get(), 1200);
    SSL_set_options(ssl_.get(), SSL_OP_NO_QUERY_MTU);

    read_bio_ = BIO_new(BIO_s_mem());
    write_bio_ = BIO_new(BIO_s_mem());
    if (read_bio_ == nullptr || write_bio_ == nullptr)
    {
        if (read_bio_ != nullptr)
        {
            BIO_free(read_bio_);
            read_bio_ = nullptr;
        }
        if (write_bio_ != nullptr)
        {
            BIO_free(write_bio_);
            write_bio_ = nullptr;
        }
        close();
        return false;
    }

    BIO_set_mem_eof_return(read_bio_, -1);
    BIO_set_mem_eof_return(write_bio_, -1);
    SSL_set0_rbio(ssl_.get(), read_bio_);
    SSL_set0_wbio(ssl_.get(), write_bio_);
    SSL_set_accept_state(ssl_.get());

    spdlog::debug("webrtc dtls transport started");
    return true;
}

void dtls_transport::close()
{
    srtp_keying_material_.reset();
    ssl_.reset();
    context_.reset();
    read_bio_ = nullptr;
    write_bio_ = nullptr;
}

bool dtls_transport::handle_datagram(std::span<const std::uint8_t> packet)
{
    if (!ssl_ || read_bio_ == nullptr || packet.empty() || packet.size() > static_cast<std::size_t>(INT_MAX))
    {
        return false;
    }

    spdlog::trace("webrtc dtls datagram input size {} content_type {}", packet.size(), packet.front());
    const auto written = BIO_write(read_bio_, packet.data(), static_cast<int>(packet.size()));
    if (written != static_cast<int>(packet.size()))
    {
        return false;
    }

    const auto result = SSL_do_handshake(ssl_.get());
    if (!pump_outgoing())
    {
        return false;
    }

    if (result != 1)
    {
        const auto error = SSL_get_error(ssl_.get(), result);
        if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE)
        {
            spdlog::debug("webrtc dtls handshake failed ssl_error {}", error);
            return false;
        }
        spdlog::trace("webrtc dtls handshake pending ssl_error {}", error);
    }

    if (!connected() && SSL_is_init_finished(ssl_.get()) != 0)
    {
        return finish_handshake();
    }
    return true;
}

bool dtls_transport::handle_timeout()
{
    if (!ssl_ || connected())
    {
        return true;
    }

    if (DTLSv1_handle_timeout(ssl_.get()) < 0)
    {
        return false;
    }
    return pump_outgoing();
}

bool dtls_transport::connected() const noexcept
{
    return srtp_keying_material_.has_value();
}

bool dtls_transport::valid_sha256_fingerprint(std::string_view fingerprint)
{
    return parse_sha256_fingerprint(fingerprint).has_value();
}

std::optional<std::chrono::milliseconds> dtls_transport::timeout() const
{
    if (!ssl_ || connected())
    {
        return std::nullopt;
    }

    timeval value{};
    if (DTLSv1_get_timeout(ssl_.get(), &value) != 1)
    {
        return std::nullopt;
    }

    const auto duration =
        std::chrono::seconds(value.tv_sec) +
        std::chrono::microseconds(value.tv_usec);
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(duration);
    return milliseconds.count() > 0 ? milliseconds : std::chrono::milliseconds(1);
}

const std::optional<dtls_srtp_keying_material>& dtls_transport::srtp_keying_material() const noexcept
{
    return srtp_keying_material_;
}

bool dtls_transport::is_dtls_packet(std::span<const std::uint8_t> packet) noexcept
{
    return packet.size() >= 13U && packet.front() >= 20U && packet.front() <= 63U;
}

bool dtls_transport::finish_handshake()
{
    if (!verify_peer_fingerprint())
    {
        spdlog::debug("webrtc dtls peer fingerprint verification failed");
        return false;
    }

    auto keying_material = export_srtp_keying_material();
    if (!keying_material)
    {
        spdlog::debug("webrtc dtls srtp keying material export failed");
        return false;
    }

    spdlog::debug("webrtc dtls handshake complete srtp profile {}", keying_material->profile);
    srtp_keying_material_ = std::move(keying_material);
    return true;
}

bool dtls_transport::verify_peer_fingerprint() const
{
    const auto expected = parse_sha256_fingerprint(remote_fingerprint_);
    if (!expected || !ssl_)
    {
        return false;
    }

    std::unique_ptr<X509, decltype(&X509_free)> peer(SSL_get1_peer_certificate(ssl_.get()), &X509_free);
    if (!peer)
    {
        return false;
    }

    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_size = 0;
    if (X509_digest(peer.get(), EVP_sha256(), digest.data(), &digest_size) != 1 || digest_size != expected->size())
    {
        return false;
    }

    return std::equal(expected->begin(), expected->end(), digest.begin());
}

std::optional<dtls_srtp_keying_material> dtls_transport::export_srtp_keying_material() const
{
    if (!ssl_ || SSL_is_init_finished(ssl_.get()) == 0)
    {
        return std::nullopt;
    }

    const auto* selected = SSL_get_selected_srtp_profile(ssl_.get());
    if (selected == nullptr || selected->name == nullptr)
    {
        return std::nullopt;
    }

    const std::string profile(selected->name);
    const auto sizes = profile_size(profile);
    if (!sizes)
    {
        return std::nullopt;
    }

    const auto total_size = 2U * (sizes->key_size + sizes->salt_size);
    std::vector<std::uint8_t> raw(total_size);
    if (SSL_export_keying_material(
            ssl_.get(),
            raw.data(),
            raw.size(),
            dtls_srtp_exporter_label.data(),
            dtls_srtp_exporter_label.size(),
            nullptr,
            0,
            0) != 1)
    {
        return std::nullopt;
    }

    dtls_srtp_keying_material result{
        .profile = profile,
        .client_write_key = {},
        .client_write_salt = {},
        .server_write_key = {},
        .server_write_salt = {},
    };
    const auto* current = raw.data();
    result.client_write_key.assign(current, current + sizes->key_size);
    current += sizes->key_size;
    result.server_write_key.assign(current, current + sizes->key_size);
    current += sizes->key_size;
    result.client_write_salt.assign(current, current + sizes->salt_size);
    current += sizes->salt_size;
    result.server_write_salt.assign(current, current + sizes->salt_size);
    return result;
}

bool dtls_transport::pump_outgoing()
{
    if (write_bio_ == nullptr)
    {
        return false;
    }

    while (BIO_ctrl_pending(write_bio_) > 0)
    {
        const auto pending = BIO_ctrl_pending(write_bio_);
        if (pending == 0 || pending > static_cast<std::size_t>(INT_MAX))
        {
            return false;
        }

        std::vector<std::uint8_t> output(pending);
        const auto read = BIO_read(write_bio_, output.data(), static_cast<int>(output.size()));
        if (read <= 0 || static_cast<std::size_t>(read) != output.size())
        {
            return false;
        }

        std::size_t offset = 0;
        while (offset < output.size())
        {
            constexpr std::size_t record_header_size = 13;
            if (output.size() - offset < record_header_size)
            {
                return false;
            }

            const auto payload_size =
                (static_cast<std::size_t>(output[offset + 11U]) << 8U) |
                static_cast<std::size_t>(output[offset + 12U]);
            const auto record_size = record_header_size + payload_size;
            if (record_size > output.size() - offset)
            {
                return false;
            }

            spdlog::trace(
                "webrtc dtls datagram output size {} content_type {}",
                record_size,
                output[offset]);
            send_(std::span<const std::uint8_t>(output.data() + offset, record_size));
            offset += record_size;
        }
    }
    return true;
}

void dtls_transport::ssl_context_deleter::operator()(SSL_CTX* value) const noexcept
{
    SSL_CTX_free(value);
}

void dtls_transport::ssl_deleter::operator()(SSL* value) const noexcept
{
    SSL_free(value);
}

}    // namespace media_server
