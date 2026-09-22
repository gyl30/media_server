#include <array>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstring>
#include <optional>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

#include <boost/asio/buffer.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http.hpp>
#include <boost/crc.hpp>
#include <boost/url/parse.hpp>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/ssl.h>

#include "media/webrtc/dtls_certificate.h"
#include "media/webrtc/srtp_transport.h"
#include "tests/clients/webrtc_test_client.h"

namespace media_server::test
{
namespace
{

constexpr std::string_view client_ice_ufrag = "capacity";
constexpr std::string_view client_ice_password = "capacity-client-password";
constexpr std::uint32_t stun_magic_cookie = 0x2112a442U;

struct ssl_context_deleter
{
    void operator()(SSL_CTX* value) const noexcept { SSL_CTX_free(value); }
};

struct ssl_deleter
{
    void operator()(SSL* value) const noexcept { SSL_free(value); }
};

using ssl_context_ptr = std::unique_ptr<SSL_CTX, ssl_context_deleter>;
using ssl_ptr = std::unique_ptr<SSL, ssl_deleter>;

void append_u16(std::vector<std::uint8_t>& output, std::uint16_t value)
{
    output.push_back(static_cast<std::uint8_t>(value >> 8U));
    output.push_back(static_cast<std::uint8_t>(value));
}

void append_u32(std::vector<std::uint8_t>& output, std::uint32_t value)
{
    output.push_back(static_cast<std::uint8_t>(value >> 24U));
    output.push_back(static_cast<std::uint8_t>(value >> 16U));
    output.push_back(static_cast<std::uint8_t>(value >> 8U));
    output.push_back(static_cast<std::uint8_t>(value));
}

void set_stun_length(std::vector<std::uint8_t>& packet, std::size_t size)
{
    packet[2] = static_cast<std::uint8_t>(size >> 8U);
    packet[3] = static_cast<std::uint8_t>(size);
}

void append_stun_attribute(std::vector<std::uint8_t>& packet, std::uint16_t type, std::span<const std::uint8_t> value)
{
    append_u16(packet, type);
    append_u16(packet, static_cast<std::uint16_t>(value.size()));
    packet.insert(packet.end(), value.begin(), value.end());
    while ((packet.size() % 4U) != 0U)
    {
        packet.push_back(0);
    }
}

std::vector<std::uint8_t> make_stun_request(std::string_view username, std::string_view password, const std::array<std::uint8_t, 12>& transaction_id)
{
    std::vector<std::uint8_t> packet;
    append_u16(packet, 0x0001);
    append_u16(packet, 0);
    append_u32(packet, stun_magic_cookie);
    packet.insert(packet.end(), transaction_id.begin(), transaction_id.end());
    append_stun_attribute(packet, 0x0006, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(username.data()), username.size()));
    constexpr std::array<std::uint8_t, 4> priority{0x6e, 0x7f, 0xff, 0xff};
    append_stun_attribute(packet, 0x0024, priority);
    constexpr std::array<std::uint8_t, 8> tie_breaker{0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    append_stun_attribute(packet, 0x802a, tie_breaker);
    append_stun_attribute(packet, 0x0025, {});

    set_stun_length(packet, packet.size() - 20U + 24U);
    std::array<std::uint8_t, 20> digest{};
    unsigned int digest_size = 0;
    if (HMAC(EVP_sha1(), password.data(), static_cast<int>(password.size()), packet.data(), packet.size(), digest.data(), &digest_size) == nullptr ||
        digest_size != digest.size())
    {
        return {};
    }
    append_stun_attribute(packet, 0x0008, digest);

    set_stun_length(packet, packet.size() - 20U + 8U);
    boost::crc_32_type crc;
    crc.process_bytes(packet.data(), packet.size());
    const auto fingerprint = crc.checksum() ^ 0x5354554eU;
    const std::array<std::uint8_t, 4> fingerprint_bytes{
        static_cast<std::uint8_t>(fingerprint >> 24U),
        static_cast<std::uint8_t>(fingerprint >> 16U),
        static_cast<std::uint8_t>(fingerprint >> 8U),
        static_cast<std::uint8_t>(fingerprint),
    };
    append_stun_attribute(packet, 0x8028, fingerprint_bytes);
    return packet;
}

std::string sdp_attribute(std::string_view sdp, std::string_view name)
{
    const auto prefix = "a=" + std::string(name) + ":";
    const auto begin = sdp.find(prefix);
    if (begin == std::string_view::npos)
    {
        return {};
    }
    const auto value_begin = begin + prefix.size();
    const auto end = sdp.find("\r\n", value_begin);
    return std::string(sdp.substr(value_begin, end == std::string_view::npos ? sdp.size() - value_begin : end - value_begin));
}

std::optional<boost::asio::ip::udp::endpoint> sdp_candidate(std::string_view sdp)
{
    constexpr std::string_view prefix = "a=candidate:";
    const auto begin = sdp.find(prefix);
    if (begin == std::string_view::npos)
    {
        return std::nullopt;
    }
    const auto end = sdp.find("\r\n", begin);
    std::istringstream fields(std::string(sdp.substr(begin + prefix.size(), end - begin - prefix.size())));
    std::string foundation;
    unsigned int component{};
    std::string protocol;
    std::uint64_t priority{};
    std::string address;
    unsigned int port{};
    std::string type_name;
    std::string type;
    fields >> foundation >> component >> protocol >> priority >> address >> port >> type_name >> type;
    boost::system::error_code error;
    const auto parsed_address = boost::asio::ip::make_address(address, error);
    if (!fields || error || component != 1U || protocol != "UDP" || port == 0U || port > 65'535U || type_name != "typ" || type != "host")
    {
        return std::nullopt;
    }
    return boost::asio::ip::udp::endpoint(parsed_address, static_cast<std::uint16_t>(port));
}

std::string certificate_fingerprint(X509* certificate)
{
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int size = 0;
    if (certificate == nullptr || X509_digest(certificate, EVP_sha256(), digest.data(), &size) != 1 || size == 0U)
    {
        return {};
    }
    std::string result;
    std::array<char, 3> byte{};
    for (unsigned int index = 0; index < size; ++index)
    {
        if (index != 0U)
        {
            result.push_back(':');
        }
        std::snprintf(byte.data(), byte.size(), "%02X", digest[index]);
        result.append(byte.data(), 2U);
    }
    return result;
}

std::optional<std::vector<std::vector<std::uint8_t>>> take_dtls_output(SSL* ssl)
{
    BIO* output_bio = SSL_get_wbio(ssl);
    std::vector<std::vector<std::uint8_t>> packets;
    while (BIO_ctrl_pending(output_bio) > 0)
    {
        const auto pending = BIO_ctrl_pending(output_bio);
        if (pending == 0U || pending > static_cast<std::size_t>(INT_MAX))
        {
            return std::nullopt;
        }
        std::vector<std::uint8_t> output(pending);
        if (BIO_read(output_bio, output.data(), static_cast<int>(output.size())) != static_cast<int>(output.size()))
        {
            return std::nullopt;
        }
        std::size_t offset = 0;
        while (offset < output.size())
        {
            constexpr std::size_t header_size = 13U;
            if (output.size() - offset < header_size)
            {
                return std::nullopt;
            }
            const auto payload_size = (static_cast<std::size_t>(output[offset + 11U]) << 8U) | output[offset + 12U];
            const auto record_size = header_size + payload_size;
            if (record_size > output.size() - offset)
            {
                return std::nullopt;
            }
            packets.emplace_back(output.begin() + static_cast<std::ptrdiff_t>(offset),
                                 output.begin() + static_cast<std::ptrdiff_t>(offset + record_size));
            offset += record_size;
        }
    }
    return packets;
}

std::optional<dtls_srtp_keying_material> make_client_srtp_material(SSL* ssl)
{
    const auto* profile = SSL_get_selected_srtp_profile(ssl);
    if (profile == nullptr || profile->name == nullptr)
    {
        return std::nullopt;
    }
    std::size_t key_size = 0;
    std::size_t salt_size = 0;
    const std::string_view name(profile->name);
    if (name == "SRTP_AEAD_AES_128_GCM")
    {
        key_size = 16U;
        salt_size = 12U;
    }
    else if (name == "SRTP_AEAD_AES_256_GCM")
    {
        key_size = 32U;
        salt_size = 12U;
    }
    else if (name == "SRTP_AES128_CM_SHA1_80")
    {
        key_size = 16U;
        salt_size = 14U;
    }
    else
    {
        return std::nullopt;
    }

    std::vector<std::uint8_t> material(2U * (key_size + salt_size));
    constexpr std::string_view label = "EXTRACTOR-dtls_srtp";
    if (SSL_export_keying_material(ssl, material.data(), material.size(), label.data(), label.size(), nullptr, 0, 0) != 1)
    {
        return std::nullopt;
    }
    const auto client_key = material.begin();
    const auto server_key = client_key + static_cast<std::ptrdiff_t>(key_size);
    const auto client_salt = server_key + static_cast<std::ptrdiff_t>(key_size);
    const auto server_salt = client_salt + static_cast<std::ptrdiff_t>(salt_size);
    return dtls_srtp_keying_material{
        .profile = std::string(name),
        .client_write_key = std::vector<std::uint8_t>(server_key, client_salt),
        .client_write_salt = std::vector<std::uint8_t>(server_salt, material.end()),
        .server_write_key = std::vector<std::uint8_t>(client_key, server_key),
        .server_write_salt = std::vector<std::uint8_t>(client_salt, server_salt),
    };
}

bool stun_response_matches(std::span<const std::uint8_t> packet, const std::array<std::uint8_t, 12>& transaction_id)
{
    return packet.size() >= 20U && packet[0] == 0x01U && packet[1] == 0x01U && packet[4] == 0x21U && packet[5] == 0x12U && packet[6] == 0xa4U &&
           packet[7] == 0x42U && std::equal(transaction_id.begin(), transaction_id.end(), packet.begin() + 8);
}

}    // namespace

struct webrtc_test_context::implementation
{
    std::shared_ptr<dtls_certificate> certificate;
    ssl_context_ptr ssl_context;
};

std::shared_ptr<webrtc_test_context> webrtc_test_context::create()
{
    auto certificate = dtls_certificate::create();
    ssl_context_ptr ssl_context(SSL_CTX_new(DTLS_method()));
    if (!certificate || !ssl_context || SSL_CTX_set_min_proto_version(ssl_context.get(), DTLS1_2_VERSION) != 1 ||
        SSL_CTX_set_max_proto_version(ssl_context.get(), DTLS1_2_VERSION) != 1 ||
        SSL_CTX_set_tlsext_use_srtp(ssl_context.get(), "SRTP_AEAD_AES_128_GCM") != 0 ||
        SSL_CTX_use_certificate(ssl_context.get(), certificate->certificate()) != 1 ||
        SSL_CTX_use_PrivateKey(ssl_context.get(), certificate->private_key()) != 1)
    {
        return {};
    }
    SSL_CTX_set_verify(ssl_context.get(), SSL_VERIFY_NONE, nullptr);
    return std::shared_ptr<webrtc_test_context>(
        new webrtc_test_context(std::make_unique<implementation>(implementation{std::move(certificate), std::move(ssl_context)})));
}

webrtc_test_context::webrtc_test_context(std::unique_ptr<implementation> state) : implementation_(std::move(state)) {}

webrtc_test_context::~webrtc_test_context() = default;

std::string webrtc_test_context::make_offer(webrtc_test_direction direction) const
{
    const auto media_direction = direction == webrtc_test_direction::publish ? "sendonly" : "recvonly";
    std::ostringstream sdp;
    sdp << "v=0\r\n"
        << "o=- 1000 2 IN IP4 127.0.0.1\r\n"
        << "s=-\r\n"
        << "t=0 0\r\n"
        << "a=group:BUNDLE 0 1\r\n"
        << "m=video 9 UDP/TLS/RTP/SAVPF 102\r\n"
        << "c=IN IP4 0.0.0.0\r\n"
        << "a=ice-ufrag:" << client_ice_ufrag << "\r\n"
        << "a=ice-pwd:" << client_ice_password << "\r\n"
        << "a=fingerprint:sha-256 " << implementation_->certificate->sha256_fingerprint() << "\r\n"
        << "a=setup:actpass\r\n"
        << "a=mid:0\r\n"
        << "a=extmap:4 urn:ietf:params:rtp-hdrext:sdes:mid\r\n"
        << "a=" << media_direction << "\r\n"
        << "a=rtcp-mux\r\n"
        << "a=rtpmap:102 H264/90000\r\n"
        << "a=fmtp:102 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f\r\n"
        << "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
        << "c=IN IP4 0.0.0.0\r\n"
        << "a=ice-ufrag:" << client_ice_ufrag << "\r\n"
        << "a=ice-pwd:" << client_ice_password << "\r\n"
        << "a=fingerprint:sha-256 " << implementation_->certificate->sha256_fingerprint() << "\r\n"
        << "a=setup:actpass\r\n"
        << "a=mid:1\r\n"
        << "a=extmap:4 urn:ietf:params:rtp-hdrext:sdes:mid\r\n"
        << "a=" << media_direction << "\r\n"
        << "a=rtcp-mux\r\n"
        << "a=rtpmap:111 opus/48000/2\r\n"
        << "a=fmtp:111 minptime=10;useinbandfec=1;stereo=1\r\n";
    return sdp.str();
}

struct webrtc_test_peer::implementation
{
    implementation(boost::asio::io_context& io, std::shared_ptr<webrtc_test_context> context_value)
        : socket(io, boost::asio::ip::udp::endpoint(boost::asio::ip::address_v4::any(), 0)), context(std::move(context_value))
    {
    }

    void receive(std::shared_ptr<webrtc_test_peer> owner)
    {
        socket.async_receive_from(
            boost::asio::buffer(receive_buffer),
            receive_sender,
            [this, owner = std::move(owner)](const boost::system::error_code& error, std::size_t size)
            {
                if (error)
                {
                    if (!closed && error_callback)
                    {
                        error_callback(error);
                    }
                    return;
                }
                if (receive_sender == server_endpoint && size != 0U && srtp_transport::is_rtp_or_rtcp({receive_buffer.data(), size}))
                {
                    ++received_media_datagrams;
                    const bool rtcp = receive_buffer[1] >= 192U && receive_buffer[1] <= 223U;
                    auto clear = rtcp ? srtp.unprotect_rtcp({receive_buffer.data(), size}) : srtp.unprotect_rtp({receive_buffer.data(), size});
                    if (clear && packet_callback)
                    {
                        packet_callback(rtcp, *clear);
                    }
                    else if (!clear)
                    {
                        ++unprotect_failures;
                    }
                }
                if (!closed)
                {
                    receive(owner);
                }
            });
    }

    boost::asio::ip::udp::socket socket;
    std::shared_ptr<webrtc_test_context> context;
    boost::asio::ip::udp::endpoint server_endpoint;
    ssl_ptr ssl;
    srtp_transport srtp;
    std::array<std::uint8_t, 4096> receive_buffer{};
    boost::asio::ip::udp::endpoint receive_sender;
    packet_handler packet_callback;
    error_handler error_callback;
    bool established{};
    bool closed{};
    std::atomic_uint64_t received_media_datagrams{};
    std::atomic_uint64_t unprotect_failures{};
};

webrtc_test_peer::webrtc_test_peer(boost::asio::io_context& io, std::shared_ptr<webrtc_test_context> context)
    : implementation_(std::make_unique<implementation>(io, std::move(context)))
{
}

webrtc_test_peer::~webrtc_test_peer() = default;

bool webrtc_test_peer::establish(std::string_view answer_sdp, std::string& error)
{
    const auto ice_ufrag = sdp_attribute(answer_sdp, "ice-ufrag");
    const auto ice_password = sdp_attribute(answer_sdp, "ice-pwd");
    const auto fingerprint = sdp_attribute(answer_sdp, "fingerprint");
    const auto endpoint = sdp_candidate(answer_sdp);
    if (ice_ufrag.empty() || ice_password.empty() || !fingerprint.starts_with("sha-256 ") || !endpoint)
    {
        error = "invalid SDP answer transport";
        return false;
    }
    implementation_->server_endpoint = *endpoint;

    const std::array<std::uint8_t, 12> transaction_id{9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 1, 2};
    const auto stun = make_stun_request(ice_ufrag + ":" + std::string(client_ice_ufrag), ice_password, transaction_id);
    boost::system::error_code socket_error;
    implementation_->socket.non_blocking(true, socket_error);
    if (socket_error || stun.empty())
    {
        error = socket_error ? socket_error.message() : "STUN request construction failed";
        return false;
    }
    implementation_->socket.send_to(boost::asio::buffer(stun), implementation_->server_endpoint, 0, socket_error);
    if (socket_error)
    {
        error = "STUN send: " + socket_error.message();
        return false;
    }
    std::array<std::uint8_t, 4096> datagram{};
    std::vector<std::vector<std::uint8_t>> early_media;
    boost::asio::ip::udp::endpoint sender;
    const auto stun_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    bool nominated = false;
    while (std::chrono::steady_clock::now() < stun_deadline)
    {
        socket_error.clear();
        const auto size = implementation_->socket.receive_from(boost::asio::buffer(datagram), sender, 0, socket_error);
        if (!socket_error && sender == implementation_->server_endpoint && stun_response_matches({datagram.data(), size}, transaction_id))
        {
            nominated = true;
            break;
        }
        if (socket_error != boost::asio::error::would_block && socket_error != boost::asio::error::try_again)
        {
            error = "STUN receive: " + socket_error.message();
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!nominated)
    {
        error = "STUN nomination timeout";
        return false;
    }

    implementation_->ssl.reset(SSL_new(implementation_->context->implementation_->ssl_context.get()));
    if (!implementation_->ssl)
    {
        error = "DTLS SSL_new failed";
        return false;
    }
    BIO* read_bio = BIO_new(BIO_s_mem());
    BIO* write_bio = BIO_new(BIO_s_mem());
    if (read_bio == nullptr || write_bio == nullptr)
    {
        BIO_free(read_bio);
        BIO_free(write_bio);
        error = "DTLS BIO allocation failed";
        return false;
    }
    BIO_set_mem_eof_return(read_bio, -1);
    BIO_set_mem_eof_return(write_bio, -1);
    SSL_set0_rbio(implementation_->ssl.get(), read_bio);
    SSL_set0_wbio(implementation_->ssl.get(), write_bio);
    SSL_set_mtu(implementation_->ssl.get(), 1200);
    SSL_set_options(implementation_->ssl.get(), SSL_OP_NO_QUERY_MTU);
    SSL_set_connect_state(implementation_->ssl.get());

    const auto dtls_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < dtls_deadline && SSL_is_init_finished(implementation_->ssl.get()) == 0)
    {
        const auto result = SSL_do_handshake(implementation_->ssl.get());
        if (result != 1)
        {
            const auto ssl_error = SSL_get_error(implementation_->ssl.get(), result);
            if (ssl_error != SSL_ERROR_WANT_READ && ssl_error != SSL_ERROR_WANT_WRITE)
            {
                error = "DTLS handshake failed";
                return false;
            }
        }
        const auto output = take_dtls_output(implementation_->ssl.get());
        if (!output)
        {
            error = "DTLS output framing failed";
            return false;
        }
        for (const auto& packet : *output)
        {
            implementation_->socket.send_to(boost::asio::buffer(packet), implementation_->server_endpoint, 0, socket_error);
            if (socket_error)
            {
                error = "DTLS send: " + socket_error.message();
                return false;
            }
        }
        while (true)
        {
            socket_error.clear();
            const auto size = implementation_->socket.receive_from(boost::asio::buffer(datagram), sender, 0, socket_error);
            if (socket_error == boost::asio::error::would_block || socket_error == boost::asio::error::try_again)
            {
                break;
            }
            if (socket_error || sender != implementation_->server_endpoint || size == 0U)
            {
                error = socket_error ? "DTLS receive: " + socket_error.message() : "DTLS input failed";
                return false;
            }
            const std::span<const std::uint8_t> packet{datagram.data(), size};
            if (dtls_transport::is_dtls_packet(packet))
            {
                if (BIO_write(read_bio, datagram.data(), static_cast<int>(size)) != static_cast<int>(size))
                {
                    error = "DTLS input failed";
                    return false;
                }
            }
            else if (srtp_transport::is_rtp_or_rtcp(packet))
            {
                early_media.emplace_back(packet.begin(), packet.end());
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (SSL_is_init_finished(implementation_->ssl.get()) == 0)
    {
        error = "DTLS handshake timeout";
        return false;
    }

    std::unique_ptr<X509, decltype(&X509_free)> peer_certificate(SSL_get1_peer_certificate(implementation_->ssl.get()), &X509_free);
    if (!peer_certificate || fingerprint != "sha-256 " + certificate_fingerprint(peer_certificate.get()))
    {
        error = "DTLS certificate fingerprint mismatch";
        return false;
    }
    const auto material = make_client_srtp_material(implementation_->ssl.get());
    if (!material || !implementation_->srtp.startup(*material))
    {
        error = "SRTP startup failed";
        return false;
    }
    for (const auto& packet : early_media)
    {
        const bool rtcp = packet[1] >= 192U && packet[1] <= 223U;
        const auto clear = rtcp ? implementation_->srtp.unprotect_rtcp(packet) : implementation_->srtp.unprotect_rtp(packet);
        if (!clear)
        {
            error = "early SRTP validation failed";
            return false;
        }
    }
    implementation_->socket.non_blocking(false, socket_error);
    if (socket_error)
    {
        error = socket_error.message();
        return false;
    }
    implementation_->established = true;
    return true;
}

void webrtc_test_peer::start_receive(packet_handler packet, error_handler error)
{
    implementation_->packet_callback = std::move(packet);
    implementation_->error_callback = std::move(error);
    implementation_->receive(shared_from_this());
}

bool webrtc_test_peer::send_rtp(std::span<const std::uint8_t> packet)
{
    auto protected_packet = implementation_->srtp.protect_rtp(packet);
    if (!protected_packet)
    {
        return false;
    }
    boost::system::error_code error;
    implementation_->socket.send_to(boost::asio::buffer(*protected_packet), implementation_->server_endpoint, 0, error);
    return !error;
}

bool webrtc_test_peer::send_rtcp(std::span<const std::uint8_t> packet)
{
    auto protected_packet = implementation_->srtp.protect_rtcp(packet);
    if (!protected_packet)
    {
        return false;
    }
    boost::system::error_code error;
    implementation_->socket.send_to(boost::asio::buffer(*protected_packet), implementation_->server_endpoint, 0, error);
    return !error;
}

void webrtc_test_peer::close() noexcept
{
    implementation_->closed = true;
    boost::system::error_code error;
    implementation_->socket.close(error);
}

std::uint16_t webrtc_test_peer::local_port() const { return implementation_->socket.local_endpoint().port(); }

std::uint64_t webrtc_test_peer::received_media_datagrams() const noexcept
{
    return implementation_->received_media_datagrams.load(std::memory_order_relaxed);
}

std::uint64_t webrtc_test_peer::unprotect_failures() const noexcept { return implementation_->unprotect_failures.load(std::memory_order_relaxed); }

bool post_webrtc_offer(std::string_view url, std::string_view offer, std::string_view stream_id, webrtc_http_response& response, std::string& error)
{
    namespace http = boost::beast::http;
    const auto parsed = boost::urls::parse_uri(url);
    if (!parsed || parsed->scheme() != "http" || parsed->host_address().empty() || parsed->has_userinfo() || parsed->has_fragment())
    {
        error = "invalid HTTP WebRTC URL";
        return false;
    }
    const auto host = std::string(parsed->host_address());
    const auto port = parsed->has_port() ? parsed->port_number() : static_cast<std::uint16_t>(80);
    const auto target = std::string(parsed->encoded_target());

    boost::asio::io_context io;
    boost::asio::ip::tcp::resolver resolver(io);
    boost::beast::tcp_stream stream(io);
    boost::system::error_code operation_error;
    const auto endpoints = resolver.resolve(host, std::to_string(port), operation_error);
    if (operation_error)
    {
        error = "HTTP resolve: " + operation_error.message();
        return false;
    }
    stream.expires_after(std::chrono::seconds(5));
    stream.connect(endpoints, operation_error);
    if (operation_error)
    {
        error = "HTTP connect: " + operation_error.message();
        return false;
    }
    http::request<http::string_body> request{http::verb::post, target, 11};
    request.set(http::field::host, parsed->host());
    request.set(http::field::content_type, "application/sdp");
    if (!stream_id.empty())
    {
        request.set("X-Stream-ID", stream_id);
    }
    request.body() = offer;
    request.prepare_payload();
    http::write(stream, request, operation_error);
    if (operation_error)
    {
        error = "HTTP write: " + operation_error.message();
        return false;
    }
    boost::beast::flat_buffer buffer;
    http::response<http::string_body> http_response;
    http::read(stream, buffer, http_response, operation_error);
    if (operation_error)
    {
        error = "HTTP read: " + operation_error.message();
        return false;
    }
    response.status = http_response.result_int();
    response.body = std::move(http_response.body());
    response.location = std::string(http_response[http::field::location]);
    if (response.location.starts_with('/'))
    {
        response.location = "http://" + std::string(parsed->encoded_authority()) + response.location;
    }
    return true;
}

bool delete_webrtc_resource(std::string_view url, std::string& error)
{
    namespace http = boost::beast::http;
    const auto parsed = boost::urls::parse_uri(url);
    if (!parsed || parsed->scheme() != "http" || parsed->host_address().empty() || parsed->has_userinfo() || parsed->has_fragment())
    {
        error = "invalid HTTP WebRTC resource URL";
        return false;
    }
    const auto host = std::string(parsed->host_address());
    const auto port = parsed->has_port() ? parsed->port_number() : static_cast<std::uint16_t>(80);
    boost::asio::io_context io;
    boost::asio::ip::tcp::resolver resolver(io);
    boost::beast::tcp_stream stream(io);
    boost::system::error_code operation_error;
    const auto endpoints = resolver.resolve(host, std::to_string(port), operation_error);
    if (operation_error)
    {
        error = "HTTP resolve: " + operation_error.message();
        return false;
    }
    stream.expires_after(std::chrono::seconds(5));
    stream.connect(endpoints, operation_error);
    if (operation_error)
    {
        error = "HTTP connect: " + operation_error.message();
        return false;
    }
    http::request<http::empty_body> request{http::verb::delete_, std::string(parsed->encoded_target()), 11};
    request.set(http::field::host, parsed->host());
    http::write(stream, request, operation_error);
    if (operation_error)
    {
        error = "HTTP DELETE write: " + operation_error.message();
        return false;
    }
    boost::beast::flat_buffer buffer;
    http::response<http::empty_body> response;
    http::read(stream, buffer, response, operation_error);
    if (operation_error)
    {
        error = "HTTP DELETE read: " + operation_error.message();
        return false;
    }
    if (response.result() != http::status::no_content && response.result() != http::status::not_found)
    {
        error = "HTTP DELETE status " + std::to_string(response.result_int());
        return false;
    }
    return true;
}

}    // namespace media_server::test
