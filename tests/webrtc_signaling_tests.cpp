#include "media/core/media_stream.h"
#include "media/core/stream_registry.h"
#include "media/hls/hls_service.h"
#include "media/http/http_session.h"
#include "media/webrtc/dtls_certificate.h"
#include "media/webrtc/rtcp_receiver.h"
#include "media/webrtc/srtp_transport.h"
#include "media/webrtc/webrtc_sdp.h"
#include "media/webrtc/whep_service.h"
#include "media/webrtc/whep_session.h"

#include <boost/asio/buffer.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>
#include <boost/crc.hpp>

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/ssl.h>
#include <openssl/srtp.h>

#include <array>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace media_server
{
namespace
{

constexpr track_id video_track_id = 1;
constexpr track_id audio_track_id = 2;

const std::vector<std::uint8_t> h264_config{
    0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0xc0, 0x1f, 0xda, 0x01, 0xe0, 0x08, 0x9f,
    0x97, 0x01, 0x6e, 0x40, 0x00, 0x00, 0x00, 0x01, 0x68, 0xce, 0x3c, 0x80,
};

const std::vector<std::uint8_t> h265_config{
    0x00, 0x00, 0x00, 0x01, 0x40, 0x01, 0x0c, 0x01, 0xff, 0xff, 0x01, 0x60, 0x00, 0x00, 0x03, 0x00, 0x80, 0x00, 0x00, 0x03, 0x00,
    0x00, 0x03, 0x00, 0x78, 0x9d, 0xc0, 0x90, 0x00, 0x00, 0x00, 0x01, 0x42, 0x01, 0x01, 0x01, 0x60, 0x00, 0x00, 0x03, 0x00, 0x80,
    0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x78, 0xa0, 0x03, 0xc0, 0x80, 0x32, 0x16, 0x59, 0xde, 0x49, 0x1b, 0x6b, 0x80, 0x40,
    0x00, 0x00, 0xfa, 0x00, 0x00, 0x17, 0x70, 0x02, 0x00, 0x00, 0x00, 0x01, 0x44, 0x01, 0xc1, 0x73, 0xd1, 0x89,
};

const std::vector<std::uint8_t> aac_asc{0x12, 0x10};

const std::vector<std::vector<std::uint8_t>> valid_aac_adts_frames{
    {0xff, 0xf1, 0x50, 0x80, 0x03, 0xdf, 0xfc, 0xde, 0x02, 0x00, 0x4c, 0x61, 0x76, 0x63, 0x36,
     0x31, 0x2e, 0x31, 0x39, 0x2e, 0x31, 0x30, 0x31, 0x00, 0x42, 0x20, 0x08, 0xc1, 0x18, 0x38},
    {0xff, 0xf1, 0x50, 0x80, 0x01, 0xbf, 0xfc, 0x21, 0x10, 0x04, 0x60, 0x8c, 0x1c},
    {0xff, 0xf1, 0x50, 0x80, 0x01, 0xbf, 0xfc, 0x21, 0x10, 0x04, 0x60, 0x8c, 0x1c},
    {0xff, 0xf1, 0x50, 0x80, 0x01, 0xbf, 0xfc, 0x21, 0x10, 0x04, 0x60, 0x8c, 0x1c},
};

const std::string webrtc_offer_sdp =
    "v=0\r\n"
    "o=- 1000 2 IN IP4 127.0.0.1\r\n"
    "s=-\r\n"
    "t=0 0\r\n"
    "a=group:BUNDLE 0 1\r\n"
    "m=video 9 UDP/TLS/RTP/SAVPF 102 127\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=ice-ufrag:remotevideo\r\n"
    "a=ice-pwd:remotevideopassword123456\r\n"
    "a=fingerprint:sha-256 00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF\r\n"
    "a=setup:actpass\r\n"
    "a=mid:0\r\n"
    "a=recvonly\r\n"
    "a=rtcp-mux\r\n"
    "a=rtpmap:102 H264/90000\r\n"
    "a=fmtp:102 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f\r\n"
    "a=rtpmap:127 rtx/90000\r\n"
    "a=fmtp:127 apt=102\r\n"
    "m=audio 9 UDP/TLS/RTP/SAVPF 111 0 8\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=ice-ufrag:remoteaudio\r\n"
    "a=ice-pwd:remoteaudiopassword123456\r\n"
    "a=fingerprint:sha-256 00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF\r\n"
    "a=setup:actpass\r\n"
    "a=mid:1\r\n"
    "a=recvonly\r\n"
    "a=rtcp-mux\r\n"
    "a=rtpmap:111 opus/48000/2\r\n"
    "a=fmtp:111 minptime=10;useinbandfec=1;stereo=1\r\n"
    "a=rtpmap:0 PCMU/8000\r\n"
    "a=rtpmap:8 PCMA/8000\r\n";

std::string offer_with_fingerprint(std::string_view fingerprint)
{
    std::string result = webrtc_offer_sdp;
    const std::string original = "00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF";
    std::size_t offset = 0;
    while ((offset = result.find(original, offset)) != std::string::npos)
    {
        result.replace(offset, original.size(), fingerprint);
        offset += fingerprint.size();
    }
    return result;
}

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

struct dtls_test_client
{
    ssl_context_ptr context;
    ssl_ptr ssl;
    BIO* read_bio{};
    BIO* write_bio{};
};

std::optional<dtls_test_client> make_dtls_test_client(const std::shared_ptr<dtls_certificate>& certificate)
{
    dtls_test_client client{
        .context = ssl_context_ptr(SSL_CTX_new(DTLS_method())),
        .ssl = {},
        .read_bio = nullptr,
        .write_bio = nullptr,
    };
    if (!client.context || SSL_CTX_set_min_proto_version(client.context.get(), DTLS1_2_VERSION) != 1 ||
        SSL_CTX_set_max_proto_version(client.context.get(), DTLS1_2_VERSION) != 1 ||
        SSL_CTX_set_tlsext_use_srtp(client.context.get(), "SRTP_AEAD_AES_128_GCM:SRTP_AES128_CM_SHA1_80") != 0 ||
        SSL_CTX_use_certificate(client.context.get(), certificate->certificate()) != 1 ||
        SSL_CTX_use_PrivateKey(client.context.get(), certificate->private_key()) != 1)
    {
        return std::nullopt;
    }

    SSL_CTX_set_verify(client.context.get(), SSL_VERIFY_NONE, nullptr);
    client.ssl.reset(SSL_new(client.context.get()));
    if (!client.ssl)
    {
        return std::nullopt;
    }

    client.read_bio = BIO_new(BIO_s_mem());
    client.write_bio = BIO_new(BIO_s_mem());
    if (client.read_bio == nullptr || client.write_bio == nullptr)
    {
        if (client.read_bio != nullptr)
        {
            BIO_free(client.read_bio);
        }
        if (client.write_bio != nullptr)
        {
            BIO_free(client.write_bio);
        }
        return std::nullopt;
    }

    BIO_set_mem_eof_return(client.read_bio, -1);
    BIO_set_mem_eof_return(client.write_bio, -1);
    SSL_set0_rbio(client.ssl.get(), client.read_bio);
    SSL_set0_wbio(client.ssl.get(), client.write_bio);
    SSL_set_mtu(client.ssl.get(), 1200);
    SSL_set_options(client.ssl.get(), SSL_OP_NO_QUERY_MTU);
    SSL_set_connect_state(client.ssl.get());
    return client;
}

bool send_dtls_client_output(dtls_test_client& client, boost::asio::ip::udp::socket& socket, const boost::asio::ip::udp::endpoint& server_endpoint)
{
    while (BIO_ctrl_pending(client.write_bio) > 0)
    {
        const auto pending = BIO_ctrl_pending(client.write_bio);
        if (pending == 0 || pending > static_cast<std::size_t>(INT_MAX))
        {
            return false;
        }

        std::vector<std::uint8_t> output(pending);
        const auto read = BIO_read(client.write_bio, output.data(), static_cast<int>(output.size()));
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
            const auto payload_size = (static_cast<std::size_t>(output[offset + 11U]) << 8U) | static_cast<std::size_t>(output[offset + 12U]);
            const auto record_size = record_header_size + payload_size;
            if (record_size > output.size() - offset)
            {
                return false;
            }
            static_cast<void>(socket.send_to(boost::asio::buffer(output.data() + offset, record_size), server_endpoint));
            offset += record_size;
        }
    }
    return true;
}

bool drive_dtls_client(boost::asio::io_context& io,
                       boost::asio::ip::udp::socket& socket,
                       const boost::asio::ip::udp::endpoint& server_endpoint,
                       whep_session& session,
                       dtls_test_client& client)
{
    boost::system::error_code error;
    socket.non_blocking(true, error);
    if (error)
    {
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    std::array<std::uint8_t, 4096> buffer{};
    boost::asio::ip::udp::endpoint sender;
    while (std::chrono::steady_clock::now() < deadline)
    {
        const auto result = SSL_do_handshake(client.ssl.get());
        if (result != 1)
        {
            const auto ssl_error = SSL_get_error(client.ssl.get(), result);
            if (ssl_error != SSL_ERROR_WANT_READ && ssl_error != SSL_ERROR_WANT_WRITE)
            {
                return false;
            }
        }
        if (!send_dtls_client_output(client, socket, server_endpoint))
        {
            return false;
        }

        io.run_for(std::chrono::milliseconds(10));
        io.restart();

        while (true)
        {
            error.clear();
            const auto size = socket.receive_from(boost::asio::buffer(buffer), sender, 0, error);
            if (error == boost::asio::error::would_block || error == boost::asio::error::try_again)
            {
                break;
            }
            if (error || sender != server_endpoint || size == 0)
            {
                return false;
            }
            if (BIO_write(client.read_bio, buffer.data(), static_cast<int>(size)) != static_cast<int>(size))
            {
                return false;
            }
        }

        if (SSL_is_init_finished(client.ssl.get()) != 0 && session.dtls_connected())
        {
            return true;
        }
    }
    return false;
}

std::optional<dtls_srtp_keying_material> make_peer_srtp_material(SSL* ssl)
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
        key_size = 16;
        salt_size = 12;
    }
    else if (name == "SRTP_AES128_CM_SHA1_80")
    {
        key_size = 16;
        salt_size = 14;
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

[[noreturn]] void fail(std::string_view message)
{
    std::cerr << "[fail] " << message << '\n';
    std::exit(1);
}

void require(bool condition, std::string_view message)
{
    if (!condition)
    {
        fail(message);
    }
}

std::string make_h265_offer(std::string offer)
{
    constexpr std::string_view h264_rtpmap = "a=rtpmap:102 H264/90000\r\n";
    constexpr std::string_view h264_fmtp = "a=fmtp:102 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f\r\n";
    const auto rtpmap_offset = offer.find(h264_rtpmap);
    const auto fmtp_offset = offer.find(h264_fmtp);
    require(rtpmap_offset != std::string::npos && fmtp_offset != std::string::npos, "webrtc h265 offer source");
    offer.erase(fmtp_offset, h264_fmtp.size());
    offer.replace(rtpmap_offset, h264_rtpmap.size(), "a=rtpmap:102 H265/90000\r\n");
    return offer;
}

constexpr std::uint32_t stun_magic_cookie = 0x2112a442U;

void append_u16(std::vector<std::uint8_t>& output, std::uint16_t value)
{
    output.push_back(static_cast<std::uint8_t>(value >> 8U));
    output.push_back(static_cast<std::uint8_t>(value & 0xffU));
}

void append_u32(std::vector<std::uint8_t>& output, std::uint32_t value)
{
    output.push_back(static_cast<std::uint8_t>(value >> 24U));
    output.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xffU));
    output.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
    output.push_back(static_cast<std::uint8_t>(value & 0xffU));
}

void set_stun_length(std::vector<std::uint8_t>& packet, std::size_t body_size)
{
    packet[2] = static_cast<std::uint8_t>(body_size >> 8U);
    packet[3] = static_cast<std::uint8_t>(body_size & 0xffU);
}

void append_rtcp_report_block(std::vector<std::uint8_t>& packet,
                              std::uint32_t source_ssrc,
                              std::uint8_t fraction_lost,
                              std::uint32_t cumulative_lost,
                              std::uint32_t highest_sequence,
                              std::uint32_t jitter,
                              std::uint32_t lsr,
                              std::uint32_t dlsr)
{
    append_u32(packet, source_ssrc);
    packet.push_back(fraction_lost);
    packet.push_back(static_cast<std::uint8_t>((cumulative_lost >> 16U) & 0xffU));
    packet.push_back(static_cast<std::uint8_t>((cumulative_lost >> 8U) & 0xffU));
    packet.push_back(static_cast<std::uint8_t>(cumulative_lost & 0xffU));
    append_u32(packet, highest_sequence);
    append_u32(packet, jitter);
    append_u32(packet, lsr);
    append_u32(packet, dlsr);
}

std::vector<std::uint8_t> make_rtcp_receiver_report()
{
    std::vector<std::uint8_t> packet;
    packet.push_back(0x82);
    packet.push_back(201);
    append_u16(packet, 13);
    append_u32(packet, 0x0102'0304U);
    append_rtcp_report_block(packet, 0x1112'1314U, 7, 0x00FF'FFFEU, 0x2122'2324U, 0x3132'3334U, 0x4142'4344U, 0x5152'5354U);
    append_rtcp_report_block(packet, 0x6162'6364U, 3, 5, 0x7172'7374U, 0x8182'8384U, 0x9192'9394U, 0xA1A2'A3A4U);
    return packet;
}

std::vector<std::uint8_t> make_rtcp_pli()
{
    std::vector<std::uint8_t> packet;
    packet.push_back(0x81);
    packet.push_back(206);
    append_u16(packet, 2);
    append_u32(packet, 0x0102'0304U);
    append_u32(packet, 0x1112'1314U);
    return packet;
}

std::vector<std::uint8_t> make_rtcp_nack()
{
    std::vector<std::uint8_t> packet;
    packet.push_back(0x81);
    packet.push_back(205);
    append_u16(packet, 3);
    append_u32(packet, 0x0102'0304U);
    append_u32(packet, 0x1112'1314U);
    append_u16(packet, 1234);
    append_u16(packet, 0x0003);
    return packet;
}

std::vector<std::uint8_t> make_rtcp_compound()
{
    auto packet = make_rtcp_receiver_report();
    const auto pli = make_rtcp_pli();
    packet.insert(packet.end(), pli.begin(), pli.end());
    return packet;
}

std::uint16_t read_network_u16(std::span<const std::uint8_t> data, std::size_t offset)
{
    require(offset + 2U <= data.size(), "read network u16 range");
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[offset]) << 8U) | static_cast<std::uint16_t>(data[offset + 1U]));
}

std::uint32_t read_network_u32(std::span<const std::uint8_t> data, std::size_t offset)
{
    require(offset + 4U <= data.size(), "read network u32 range");
    return (static_cast<std::uint32_t>(data[offset]) << 24U) | (static_cast<std::uint32_t>(data[offset + 1U]) << 16U) |
           (static_cast<std::uint32_t>(data[offset + 2U]) << 8U) | static_cast<std::uint32_t>(data[offset + 3U]);
}

void require_server_sender_report(std::span<const std::uint8_t> packet, std::uint32_t expected_ssrc, std::string_view expected_cname)
{
    require(packet.size() >= 40U, "server rtcp compound size");
    require((packet[0] >> 6U) == 2U, "server rtcp sr version");
    require(packet[1] == 200U, "server rtcp sender report type");
    require(read_network_u32(packet, 4U) == expected_ssrc, "server rtcp sender ssrc");
    require(read_network_u32(packet, 20U) > 0U, "server rtcp sender packet count");
    require(read_network_u32(packet, 24U) > 0U, "server rtcp sender octet count");

    const auto sr_size = (static_cast<std::size_t>(read_network_u16(packet, 2U)) + 1U) * 4U;
    require(sr_size >= 28U && sr_size + 12U <= packet.size(), "server rtcp sender report size");
    require(packet[sr_size + 1U] == 202U, "server rtcp sdes type");
    require(read_network_u32(packet, sr_size + 4U) == expected_ssrc, "server rtcp sdes ssrc");
    require(packet[sr_size + 8U] == 1U, "server rtcp cname type");

    const auto cname_size = static_cast<std::size_t>(packet[sr_size + 9U]);
    require(sr_size + 10U + cname_size <= packet.size(), "server rtcp cname range");
    const auto cname = std::string_view(reinterpret_cast<const char*>(packet.data() + sr_size + 10U), cname_size);
    require(cname == expected_cname, "server rtcp cname");
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

std::array<std::uint8_t, 20> stun_hmac(std::string_view password, std::span<const std::uint8_t> data)
{
    std::array<std::uint8_t, 20> digest{};
    unsigned int size = 0;
    const auto* result = HMAC(EVP_sha1(), password.data(), static_cast<int>(password.size()), data.data(), data.size(), digest.data(), &size);
    require(result != nullptr && size == digest.size(), "stun hmac");
    return digest;
}

std::uint32_t stun_fingerprint(std::span<const std::uint8_t> data)
{
    boost::crc_32_type crc;
    crc.process_bytes(data.data(), data.size());
    return crc.checksum() ^ 0x5354554eU;
}

std::vector<std::uint8_t> make_stun_request(std::string_view username,
                                            std::string_view password,
                                            const std::array<std::uint8_t, 12>& transaction_id,
                                            bool use_candidate)
{
    std::vector<std::uint8_t> packet;
    append_u16(packet, 0x0001);
    append_u16(packet, 0);
    append_u32(packet, stun_magic_cookie);
    packet.insert(packet.end(), transaction_id.begin(), transaction_id.end());

    append_stun_attribute(packet, 0x0006, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(username.data()), username.size()));
    if (use_candidate)
    {
        append_stun_attribute(packet, 0x0025, {});
    }

    set_stun_length(packet, packet.size() - 20U + 24U);
    const auto digest = stun_hmac(password, packet);
    append_stun_attribute(packet, 0x0008, digest);

    set_stun_length(packet, packet.size() - 20U + 8U);
    const auto fingerprint = stun_fingerprint(packet);
    const std::array<std::uint8_t, 4> fingerprint_bytes{
        static_cast<std::uint8_t>(fingerprint >> 24U),
        static_cast<std::uint8_t>((fingerprint >> 16U) & 0xffU),
        static_cast<std::uint8_t>((fingerprint >> 8U) & 0xffU),
        static_cast<std::uint8_t>(fingerprint & 0xffU),
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

std::vector<std::uint8_t> exchange_stun(boost::asio::io_context& io,
                                        boost::asio::ip::udp::socket& client,
                                        const boost::asio::ip::udp::endpoint& server_endpoint,
                                        const std::vector<std::uint8_t>& request)
{
    std::array<std::uint8_t, 2048> buffer{};
    boost::asio::ip::udp::endpoint sender;
    boost::system::error_code receive_error;
    std::size_t received = 0;
    bool complete = false;

    client.async_receive_from(boost::asio::buffer(buffer),
                              sender,
                              [&](boost::system::error_code error, std::size_t size)
                              {
                                  receive_error = error;
                                  received = size;
                                  complete = true;
                              });
    static_cast<void>(client.send_to(boost::asio::buffer(request), server_endpoint));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!complete && std::chrono::steady_clock::now() < deadline)
    {
        io.run_for(std::chrono::milliseconds(20));
        io.restart();
    }

    require(complete && !receive_error, "stun response receive");
    return {buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(received)};
}

void require_stun_success(std::span<const std::uint8_t> response, const std::array<std::uint8_t, 12>& transaction_id)
{
    require(response.size() >= 20U, "stun response size");
    require(response[0] == 0x01 && response[1] == 0x01, "stun binding success type");
    require(response[4] == 0x21 && response[5] == 0x12 && response[6] == 0xa4 && response[7] == 0x42, "stun response cookie");
    require(std::equal(transaction_id.begin(), transaction_id.end(), response.begin() + 8), "stun response transaction id");
}

media_track make_video_track()
{
    return media_track{
        .id = video_track_id,
        .kind = media_kind::video,
        .codec = codec_id::h264,
        .clock_rate = 90'000,
        .channel_count = 0,
        .codec_config = h264_config,
    };
}

media_track make_h265_track()
{
    return media_track{
        .id = video_track_id,
        .kind = media_kind::video,
        .codec = codec_id::h265,
        .clock_rate = 90'000,
        .channel_count = 0,
        .codec_config = h265_config,
    };
}

media_track make_audio_track()
{
    return media_track{
        .id = audio_track_id,
        .kind = media_kind::audio,
        .codec = codec_id::aac,
        .clock_rate = 44'100,
        .channel_count = 2,
        .codec_config = aac_asc,
    };
}

media_frame make_video_key_frame(codec_id codec)
{
    require(codec == codec_id::h264 || codec == codec_id::h265, "video key frame codec");
    std::vector<std::uint8_t> payload;
    if (codec == codec_id::h265)
    {
        payload = {0x00, 0x00, 0x00, 0x01, 0x26, 0x01, 0x9a, 0x20, 0x11};
    }
    else
    {
        payload = {0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x21, 0xa0, 0x10, 0x08, 0x04};
    }
    return media_frame{
        .track = video_track_id,
        .dts_ns = 0,
        .pts_ns = 0,
        .key_frame = true,
        .payload = std::make_shared<const std::vector<std::uint8_t>>(std::move(payload)),
    };
}

class whep_http_test_peer final
{
   public:
    whep_http_test_peer()
        : work_(boost::asio::make_work_guard(io_)),
          hls_(registry_),
          whep_(io_, registry_, boost::asio::ip::make_address("127.0.0.1")),
          acceptor_(io_, {boost::asio::ip::tcp::v4(), 0})
    {
        stream_ = std::make_shared<media_stream>("live/camera");
        require(stream_->update_track(make_video_track()), "whep http video track");
        require(stream_->update_track(make_audio_track()), "whep http audio track");
        require(registry_.add(stream_), "whep http registry add");
        require(whep_.ready(), "whep http service ready");
        runner_ = std::jthread([this]() { io_.run(); });
    }

    ~whep_http_test_peer()
    {
        work_.reset();
        io_.stop();
        runner_.join();
        whep_.close();
    }

    boost::beast::http::response<boost::beast::http::string_body> options(std::string target, std::string_view requested_method)
    {
        boost::beast::http::request<boost::beast::http::string_body> request{boost::beast::http::verb::options, std::move(target), 11};
        request.set(boost::beast::http::field::host, "127.0.0.1");
        request.set(boost::beast::http::field::origin, "https://player.example");
        request.set(boost::beast::http::field::access_control_request_method, requested_method);
        request.set(boost::beast::http::field::access_control_request_headers, "Content-Type");
        request.prepare_payload();
        return send(std::move(request));
    }

    boost::beast::http::response<boost::beast::http::string_body> post(std::string target)
    {
        boost::beast::http::request<boost::beast::http::string_body> request{boost::beast::http::verb::post, std::move(target), 11};
        request.set(boost::beast::http::field::host, "127.0.0.1");
        request.set(boost::beast::http::field::origin, "https://player.example");
        request.set(boost::beast::http::field::content_type, "application/sdp");
        request.body() = webrtc_offer_sdp;
        request.prepare_payload();
        return send(std::move(request));
    }

    boost::beast::http::response<boost::beast::http::string_body> remove(std::string target)
    {
        boost::beast::http::request<boost::beast::http::string_body> request{boost::beast::http::verb::delete_, std::move(target), 11};
        request.set(boost::beast::http::field::host, "127.0.0.1");
        request.set(boost::beast::http::field::origin, "https://player.example");
        request.prepare_payload();
        return send(std::move(request));
    }

   private:
    boost::beast::http::response<boost::beast::http::string_body> send(boost::beast::http::request<boost::beast::http::string_body> request)
    {
        boost::asio::ip::tcp::socket client(client_io_);
        client.connect(acceptor_.local_endpoint());
        auto server_socket = acceptor_.accept();
        auto session = std::make_shared<http_session>(std::move(server_socket), registry_, hls_, whep_);
        session->start();

        boost::beast::http::write(client, request);
        boost::beast::flat_buffer buffer;
        boost::beast::http::response<boost::beast::http::string_body> response;
        boost::beast::http::read(client, buffer, response);
        boost::system::error_code error;
        client.close(error);
        return response;
    }

    boost::asio::io_context io_;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work_;
    stream_registry registry_;
    hls_service hls_;
    whep_service whep_;
    boost::asio::ip::tcp::acceptor acceptor_;
    boost::asio::io_context client_io_;
    std::shared_ptr<media_stream> stream_;
    std::jthread runner_;
};

void require_whep_options(const boost::beast::http::response<boost::beast::http::string_body>& response)
{
    require(response.result() == boost::beast::http::status::ok, "whep options status");
    require(response["Access-Control-Allow-Origin"] == "*", "whep options allow origin");
    require(response["Access-Control-Allow-Methods"] == "POST, DELETE, OPTIONS", "whep options allow methods");
    require(response["Access-Control-Allow-Headers"] == "Content-Type", "whep options allow headers");
    require(response["Accept-Post"] == "application/sdp", "whep options accept post");
    require(response[boost::beast::http::field::content_length] == "0", "whep options content length");
    require(response.body().empty(), "whep options empty body");
}

void test_whep_http_cors()
{
    whep_http_test_peer peer;
    require_whep_options(peer.options("/whep/live/camera", "POST"));

    const auto created = peer.post("/whep/live/camera");
    require(created.result() == boost::beast::http::status::created, "whep cors create status");
    require(created["Access-Control-Allow-Origin"] == "*", "whep create allow origin");
    require(created["Access-Control-Expose-Headers"] == "Location", "whep create expose location");
    require(created[boost::beast::http::field::content_type] == "application/sdp", "whep create content type");
    const auto location = std::string(created[boost::beast::http::field::location]);
    require(location.starts_with("/whep/session/"), "whep create location");
    require(created.body().starts_with("v=0\r\n"), "whep create answer");

    require_whep_options(peer.options(location, "DELETE"));
    const auto removed = peer.remove(location);
    require(removed.result() == boost::beast::http::status::no_content, "whep cors delete status");
    require(removed["Access-Control-Allow-Origin"] == "*", "whep delete allow origin");

    const auto missing = peer.remove(location);
    require(missing.result() == boost::beast::http::status::not_found, "whep cors error status");
    require(missing["Access-Control-Allow-Origin"] == "*", "whep error allow origin");
}

void test_webrtc_sdp_answer()
{
    const auto offer = parse_webrtc_offer(webrtc_offer_sdp);
    require(offer.has_value(), "parse webrtc offer");

    const auto answer = make_webrtc_answer(*offer,
                                           {make_video_track(), make_audio_track()},
                                           webrtc_answer_config{
                                               .address = boost::asio::ip::make_address("127.0.0.1"),
                                               .port = 40000,
                                               .ice_ufrag = "serverufrag",
                                               .ice_pwd = "serverpassword1234567890",
                                               .fingerprint = "AA:BB:CC:DD",
                                           });
    require(answer.has_value(), "make webrtc answer");
    require(answer->video_codec == codec_id::h264, "webrtc negotiated h264 codec");
    require(answer->video_payload_type == 102, "webrtc negotiated h264 payload");
    require(answer->audio_payload_type == 111, "webrtc negotiated opus payload");
    require(answer->audio_channel_count == 2, "webrtc negotiated opus stereo");
    require(answer->sdp.find("a=ice-lite\r\n") != std::string::npos, "webrtc ice lite");
    require(answer->sdp.find("a=end-of-candidates\r\n") != std::string::npos, "webrtc complete candidates");
    require(answer->sdp.find("trickle") == std::string::npos, "webrtc no trickle");
    require(answer->sdp.find("m=video 40000 UDP/TLS/RTP/SAVPF 102\r\n") != std::string::npos, "webrtc h264 payload selection");
    require(answer->sdp.find("a=rtpmap:102 H264/90000\r\n") != std::string::npos, "webrtc h264 rtpmap");
    require(answer->sdp.find("profile-level-id=42c01f") != std::string::npos, "webrtc source h264 profile");
    require(answer->sdp.find("m=audio 40000 UDP/TLS/RTP/SAVPF 111\r\n") != std::string::npos, "webrtc opus payload selection");
    require(answer->sdp.find("a=rtpmap:111 opus/48000/2\r\n") != std::string::npos, "webrtc opus rtpmap");
    require(answer->sdp.find("sprop-stereo=1") != std::string::npos, "webrtc opus stereo sender property");
    require(answer->sdp.find("a=sendonly\r\n") != std::string::npos, "webrtc sendonly");
}

void test_webrtc_h265_sdp_answer()
{
    const auto h265_offer_sdp = make_h265_offer(webrtc_offer_sdp);
    const auto offer = parse_webrtc_offer(h265_offer_sdp);
    require(offer.has_value(), "parse webrtc h265 offer");
    const auto answer = make_webrtc_answer(*offer,
                                           {make_h265_track(), make_audio_track()},
                                           webrtc_answer_config{
                                               .address = boost::asio::ip::make_address("127.0.0.1"),
                                               .port = 40000,
                                               .ice_ufrag = "serverufrag",
                                               .ice_pwd = "serverpassword1234567890",
                                               .fingerprint = "AA:BB:CC:DD",
                                           });
    require(answer.has_value(), "make webrtc h265 answer");
    require(answer->video_codec == codec_id::h265, "webrtc negotiated h265 codec");
    require(answer->video_payload_type == 102, "webrtc negotiated h265 payload");
    require(answer->sdp.find("a=rtpmap:102 H265/90000\r\n") != std::string::npos, "webrtc h265 rtpmap");
    require(answer->sdp.find("H264/90000") == std::string::npos, "webrtc h265 answer excludes h264");
}

void test_webrtc_payload_type_membership()
{
    const auto config = webrtc_answer_config{
        .address = boost::asio::ip::make_address("127.0.0.1"),
        .port = 40000,
        .ice_ufrag = "serverufrag",
        .ice_pwd = "serverpassword1234567890",
        .fingerprint = "AA:BB:CC:DD",
    };
    const auto answer_for = [&config](const std::string& sdp, std::vector<media_track> tracks)
    {
        const auto offer = parse_webrtc_offer(sdp);
        require(offer.has_value(), "webrtc parse payload membership offer");
        return make_webrtc_answer(*offer, tracks, config);
    };

    auto h264_sdp = webrtc_offer_sdp;
    constexpr std::string_view video_formats = "m=video 9 UDP/TLS/RTP/SAVPF 102 127";
    const auto video_formats_offset = h264_sdp.find(video_formats);
    require(video_formats_offset != std::string::npos, "webrtc video payload membership source");
    h264_sdp.replace(video_formats_offset, video_formats.size(), "m=video 9 UDP/TLS/RTP/SAVPF 127");
    const auto h264_answer = answer_for(h264_sdp, {make_video_track(), make_audio_track()});
    require(h264_answer.has_value() && !h264_answer->video_payload_type.has_value(), "webrtc reject unoffered h264 payload");

    auto h265_sdp = h264_sdp;
    h265_sdp.replace(h265_sdp.find("H264/90000"), 10U, "H265/90000");
    const auto h265_answer = answer_for(h265_sdp, {make_h265_track(), make_audio_track()});
    require(h265_answer.has_value() && !h265_answer->video_payload_type.has_value(), "webrtc reject unoffered h265 payload");

    auto opus_sdp = webrtc_offer_sdp;
    constexpr std::string_view audio_formats = "m=audio 9 UDP/TLS/RTP/SAVPF 111 0 8";
    const auto audio_formats_offset = opus_sdp.find(audio_formats);
    require(audio_formats_offset != std::string::npos, "webrtc audio payload membership source");
    opus_sdp.replace(audio_formats_offset, audio_formats.size(), "m=audio 9 UDP/TLS/RTP/SAVPF 0 8");
    const auto opus_answer = answer_for(opus_sdp, {make_video_track(), make_audio_track()});
    require(opus_answer.has_value() && !opus_answer->audio_payload_type.has_value(), "webrtc reject unoffered opus payload");
}

void test_webrtc_payload_type_range()
{
    const auto config = webrtc_answer_config{
        .address = boost::asio::ip::make_address("127.0.0.1"),
        .port = 40000,
        .ice_ufrag = "serverufrag",
        .ice_pwd = "serverpassword1234567890",
        .fingerprint = "AA:BB:CC:DD",
    };
    auto invalid_sdp = webrtc_offer_sdp;
    const auto replace = [&invalid_sdp](std::string_view source, std::string_view target)
    {
        const auto offset = invalid_sdp.find(source);
        require(offset != std::string::npos, "webrtc payload range source");
        invalid_sdp.replace(offset, source.size(), target);
    };
    replace("m=video 9 UDP/TLS/RTP/SAVPF 102 127", "m=video 9 UDP/TLS/RTP/SAVPF 128");
    replace("a=rtpmap:102 H264/90000", "a=rtpmap:128 H264/90000");
    replace("a=fmtp:102 ", "a=fmtp:128 ");
    replace("m=audio 9 UDP/TLS/RTP/SAVPF 111 0 8", "m=audio 9 UDP/TLS/RTP/SAVPF 129");
    replace("a=rtpmap:111 opus/48000/2", "a=rtpmap:129 opus/48000/2");
    replace("a=fmtp:111 ", "a=fmtp:129 ");

    const auto offer = parse_webrtc_offer(invalid_sdp);
    require(offer.has_value(), "webrtc parse out of range payload offer");
    require(!make_webrtc_answer(*offer, {make_video_track()}, config).has_value(), "webrtc reject out of range h264 payload");
    require(!make_webrtc_answer(*offer, {make_audio_track()}, config).has_value(), "webrtc reject out of range opus payload");

    replace("H264/90000", "H265/90000");
    const auto h265_offer = parse_webrtc_offer(invalid_sdp);
    require(h265_offer.has_value(), "webrtc parse out of range h265 payload offer");
    require(!make_webrtc_answer(*h265_offer, {make_h265_track()}, config).has_value(), "webrtc reject out of range h265 payload");
}

void test_webrtc_disabled_media()
{
    const auto config = webrtc_answer_config{
        .address = boost::asio::ip::make_address("127.0.0.1"),
        .port = 40000,
        .ice_ufrag = "serverufrag",
        .ice_pwd = "serverpassword1234567890",
        .fingerprint = "AA:BB:CC:DD",
    };
    auto disabled_video_sdp = webrtc_offer_sdp;
    disabled_video_sdp.replace(disabled_video_sdp.find("m=video 9 "), 10U, "m=video 0 ");
    const auto disabled_video = parse_webrtc_offer(disabled_video_sdp);
    require(disabled_video.has_value(), "webrtc parse disabled video offer");
    require(!make_webrtc_answer(*disabled_video, {make_video_track()}, config).has_value(), "webrtc keep disabled h264 inactive");

    disabled_video_sdp.replace(disabled_video_sdp.find("H264/90000"), 10U, "H265/90000");
    const auto disabled_h265 = parse_webrtc_offer(disabled_video_sdp);
    require(disabled_h265.has_value(), "webrtc parse disabled h265 offer");
    require(!make_webrtc_answer(*disabled_h265, {make_h265_track()}, config).has_value(), "webrtc keep disabled h265 inactive");

    auto disabled_audio_sdp = webrtc_offer_sdp;
    disabled_audio_sdp.replace(disabled_audio_sdp.find("m=audio 9 "), 10U, "m=audio 0 ");
    const auto disabled_audio = parse_webrtc_offer(disabled_audio_sdp);
    require(disabled_audio.has_value(), "webrtc parse disabled audio offer");
    require(!make_webrtc_answer(*disabled_audio, {make_audio_track()}, config).has_value(), "webrtc keep disabled opus inactive");

    const std::string mid = "a=mid:1\r\n";
    const auto mid_offset = disabled_audio_sdp.find(mid);
    require(mid_offset != std::string::npos, "webrtc bundle only audio source");
    disabled_audio_sdp.replace(mid_offset, mid.size(), mid + "a=bundle-only\r\n");
    const auto bundle_only_audio = parse_webrtc_offer(disabled_audio_sdp);
    require(bundle_only_audio.has_value(), "webrtc parse bundle only audio offer");
    const auto bundle_only_answer = make_webrtc_answer(*bundle_only_audio, {make_video_track(), make_audio_track()}, config);
    require(bundle_only_answer.has_value() && bundle_only_answer->audio_payload_type == 111, "webrtc accept bundle only opus");
}

void test_webrtc_single_media_per_kind()
{
    auto duplicate_sdp = webrtc_offer_sdp;
    const std::string bundle = "a=group:BUNDLE 0 1\r\n";
    const auto bundle_offset = duplicate_sdp.find(bundle);
    require(bundle_offset != std::string::npos, "webrtc duplicate media bundle");
    duplicate_sdp.replace(bundle_offset, bundle.size(), "a=group:BUNDLE 0 1 2 3\r\n");
    duplicate_sdp +=
        "m=video 9 UDP/TLS/RTP/SAVPF 103\r\n"
        "c=IN IP4 0.0.0.0\r\n"
        "a=mid:2\r\n"
        "a=recvonly\r\n"
        "a=rtcp-mux\r\n"
        "a=rtpmap:103 H264/90000\r\n"
        "a=fmtp:103 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f\r\n"
        "m=audio 9 UDP/TLS/RTP/SAVPF 112\r\n"
        "c=IN IP4 0.0.0.0\r\n"
        "a=mid:3\r\n"
        "a=recvonly\r\n"
        "a=rtcp-mux\r\n"
        "a=rtpmap:112 opus/48000/2\r\n";

    const auto config = webrtc_answer_config{
        .address = boost::asio::ip::make_address("127.0.0.1"),
        .port = 40000,
        .ice_ufrag = "serverufrag",
        .ice_pwd = "serverpassword1234567890",
        .fingerprint = "AA:BB:CC:DD",
    };
    const auto offer = parse_webrtc_offer(duplicate_sdp);
    require(offer.has_value(), "webrtc parse duplicate media offer");
    const auto answer = make_webrtc_answer(*offer, {make_video_track(), make_audio_track()}, config);
    require(answer.has_value(), "webrtc answer duplicate media offer");
    require(answer->video_payload_type == 102, "webrtc accept one video media");
    require(answer->audio_payload_type == 111, "webrtc accept one audio media");
    require(answer->sdp.find("m=video 0 UDP/TLS/RTP/SAVPF 103\r\n") != std::string::npos, "webrtc reject duplicate video media");
    require(answer->sdp.find("m=audio 0 UDP/TLS/RTP/SAVPF 112\r\n") != std::string::npos, "webrtc reject duplicate audio media");

    auto h265_sdp = duplicate_sdp;
    std::size_t offset = 0;
    while ((offset = h265_sdp.find("H264/90000", offset)) != std::string::npos)
    {
        h265_sdp.replace(offset, 10U, "H265/90000");
        offset += 10U;
    }
    const auto h265_offer = parse_webrtc_offer(h265_sdp);
    require(h265_offer.has_value(), "webrtc parse duplicate h265 media offer");
    const auto h265_answer = make_webrtc_answer(*h265_offer, {make_h265_track(), make_audio_track()}, config);
    require(h265_answer.has_value(), "webrtc answer duplicate h265 media offer");
    require(h265_answer->video_codec == codec_id::h265 && h265_answer->video_payload_type == 102, "webrtc accept one h265 media");
    require(h265_answer->sdp.find("m=video 0 UDP/TLS/RTP/SAVPF 103\r\n") != std::string::npos, "webrtc reject duplicate h265 media");
}

void test_webrtc_opus_mono_default()
{
    auto mono_offer_sdp = webrtc_offer_sdp;
    const std::string stereo_parameter = ";stereo=1";
    const auto stereo_offset = mono_offer_sdp.find(stereo_parameter);
    require(stereo_offset != std::string::npos, "webrtc stereo offer parameter");
    mono_offer_sdp.erase(stereo_offset, stereo_parameter.size());

    const auto offer = parse_webrtc_offer(mono_offer_sdp);
    require(offer.has_value(), "parse webrtc mono offer");

    const auto answer = make_webrtc_answer(*offer,
                                           {make_video_track(), make_audio_track()},
                                           webrtc_answer_config{
                                               .address = boost::asio::ip::make_address("127.0.0.1"),
                                               .port = 40000,
                                               .ice_ufrag = "serverufrag",
                                               .ice_pwd = "serverpassword1234567890",
                                               .fingerprint = "AA:BB:CC:DD",
                                           });
    require(answer.has_value(), "make webrtc mono answer");
    require(answer->audio_channel_count == 1, "webrtc opus mono default");
    require(answer->sdp.find("sprop-stereo=0") != std::string::npos, "webrtc opus mono sender property");
}

void test_webrtc_transport_contract()
{
    const auto config = webrtc_answer_config{
        .address = boost::asio::ip::make_address("127.0.0.1"),
        .port = 40000,
        .ice_ufrag = "serverufrag",
        .ice_pwd = "serverpassword1234567890",
        .fingerprint = "AA:BB:CC:DD",
    };
    const auto tracks = std::vector<media_track>{make_video_track(), make_audio_track()};

    auto no_bundle_sdp = webrtc_offer_sdp;
    const std::string bundle = "a=group:BUNDLE 0 1\r\n";
    const auto no_bundle_offset = no_bundle_sdp.find(bundle);
    require(no_bundle_offset != std::string::npos, "webrtc bundle offer");
    no_bundle_sdp.erase(no_bundle_offset, bundle.size());
    const auto no_bundle = parse_webrtc_offer(no_bundle_sdp);
    require(no_bundle.has_value(), "webrtc parse unbundled offer");
    require(!make_webrtc_answer(*no_bundle, tracks, config).has_value(), "webrtc reject unbundled offer");

    auto no_rtcp_mux_sdp = webrtc_offer_sdp;
    const std::string rtcp_mux = "a=rtcp-mux\r\n";
    const auto rtcp_mux_offset = no_rtcp_mux_sdp.find(rtcp_mux);
    require(rtcp_mux_offset != std::string::npos, "webrtc rtcp mux offer");
    no_rtcp_mux_sdp.erase(rtcp_mux_offset, rtcp_mux.size());
    const auto no_rtcp_mux = parse_webrtc_offer(no_rtcp_mux_sdp);
    require(no_rtcp_mux.has_value(), "webrtc parse no rtcp mux offer");
    const auto no_rtcp_mux_answer = make_webrtc_answer(*no_rtcp_mux, tracks, config);
    require(no_rtcp_mux_answer.has_value() && no_rtcp_mux_answer->transport_mid == "1", "webrtc answer remaining rtcp mux media");
    require(!no_rtcp_mux_answer->video_payload_type.has_value() && no_rtcp_mux_answer->audio_payload_type == 111,
            "webrtc reject media without rtcp mux");

    auto wrong_protocol_sdp = webrtc_offer_sdp;
    const std::string protocol = "UDP/TLS/RTP/SAVPF";
    const auto protocol_offset = wrong_protocol_sdp.find(protocol);
    require(protocol_offset != std::string::npos, "webrtc protocol offer");
    wrong_protocol_sdp.replace(protocol_offset, protocol.size(), "RTP/AVP");
    const auto wrong_protocol = parse_webrtc_offer(wrong_protocol_sdp);
    require(wrong_protocol.has_value(), "webrtc parse wrong protocol offer");
    const auto wrong_protocol_answer = make_webrtc_answer(*wrong_protocol, tracks, config);
    require(wrong_protocol_answer.has_value() && wrong_protocol_answer->transport_mid == "1", "webrtc answer remaining dtls srtp media");
    require(!wrong_protocol_answer->video_payload_type.has_value() && wrong_protocol_answer->audio_payload_type == 111,
            "webrtc reject media without dtls srtp protocol");

    auto passive_offer_sdp = webrtc_offer_sdp;
    const std::string actpass = "a=setup:actpass\r\n";
    const auto setup_offset = passive_offer_sdp.find(actpass);
    require(setup_offset != std::string::npos, "webrtc setup offer");
    passive_offer_sdp.replace(setup_offset, actpass.size(), "a=setup:passive\r\n");
    const auto passive_offer = parse_webrtc_offer(passive_offer_sdp);
    require(passive_offer.has_value(), "webrtc parse passive setup offer");
    const auto passive_answer = make_webrtc_answer(*passive_offer, tracks, config);
    require(passive_answer.has_value() && passive_answer->transport_mid == "1", "webrtc select next actpass transport");

    auto video_only_bundle_sdp = webrtc_offer_sdp;
    const auto bundle_offset = video_only_bundle_sdp.find(bundle);
    require(bundle_offset != std::string::npos, "webrtc bundle offer");
    video_only_bundle_sdp.replace(bundle_offset, bundle.size(), "a=group:BUNDLE 0\r\n");
    const auto video_only_bundle = parse_webrtc_offer(video_only_bundle_sdp);
    require(video_only_bundle.has_value(), "webrtc parse partial bundle offer");
    const auto partial_answer = make_webrtc_answer(*video_only_bundle, tracks, config);
    require(partial_answer.has_value(), "webrtc answer bundled video");
    require(partial_answer->video_payload_type == 102, "webrtc bundled video accepted");
    require(!partial_answer->audio_payload_type.has_value(), "webrtc unbundled audio rejected");
    require(partial_answer->sdp.find("a=group:BUNDLE 0\r\n") != std::string::npos, "webrtc answer bundle mids");
    require(partial_answer->sdp.find("m=audio 0 UDP/TLS/RTP/SAVPF") != std::string::npos, "webrtc unbundled audio inactive");

    const auto audio_only_offer = parse_webrtc_offer(webrtc_offer_sdp);
    require(audio_only_offer.has_value(), "webrtc parse audio only source offer");
    const auto audio_only_answer = make_webrtc_answer(*audio_only_offer, {make_audio_track()}, config);
    require(audio_only_answer.has_value(), "webrtc answer audio only source");
    require(audio_only_answer->sdp.find("a=group:BUNDLE 1\r\n") != std::string::npos, "webrtc answer selects accepted bundle tag");
    require(audio_only_answer->sdp.find("m=video 0 UDP/TLS/RTP/SAVPF") != std::string::npos, "webrtc reject unavailable bundled video");

    auto unknown_bundle_mid_sdp = webrtc_offer_sdp;
    const auto unknown_bundle_offset = unknown_bundle_mid_sdp.find(bundle);
    require(unknown_bundle_offset != std::string::npos, "webrtc bundle mid offer");
    unknown_bundle_mid_sdp.replace(unknown_bundle_offset, bundle.size(), "a=group:BUNDLE 0 missing\r\n");
    require(!parse_webrtc_offer(unknown_bundle_mid_sdp).has_value(), "webrtc reject unknown bundle mid");
}

void test_whep_session_start_errors()
{
    boost::asio::io_context io;
    auto stream = std::make_shared<media_stream>("live/start-errors");
    require(stream->update_track(make_video_track()), "start errors video track");
    require(stream->update_track(make_audio_track()), "start errors audio track");

    const auto offer = parse_webrtc_offer(webrtc_offer_sdp);
    require(offer.has_value(), "start errors parse offer");
    auto certificate = dtls_certificate::create();
    require(certificate != nullptr, "start errors certificate");

    const auto make_session = [&](std::shared_ptr<media_stream> source, std::shared_ptr<dtls_certificate> session_certificate)
    { return std::make_shared<whep_session>(io, std::move(source), boost::asio::ip::make_address("127.0.0.1"), std::move(session_certificate)); };

    auto invalid_offer = *offer;
    require(!invalid_offer.media.empty(), "start errors media");
    invalid_offer.media.front().ice_ufrag.clear();
    require(make_session(stream, certificate)->start(std::move(invalid_offer)) == whep_session_start_error::invalid_offer,
            "start errors invalid offer");

    auto invalid_fingerprint_offer = *offer;
    invalid_fingerprint_offer.media.front().fingerprint = "invalid";
    require(make_session(stream, certificate)->start(std::move(invalid_fingerprint_offer)) == whep_session_start_error::invalid_offer,
            "start errors invalid fingerprint");

    require(make_session(stream, nullptr)->start(*offer) == whep_session_start_error::internal_error, "start errors internal error");

    auto ended_stream = std::make_shared<media_stream>("live/start-errors-ended");
    require(ended_stream->update_track(make_video_track()), "start errors ended video track");
    ended_stream->end();
    require(make_session(ended_stream, certificate)->start(*offer) == whep_session_start_error::stream_not_ready, "start errors stream not ready");

    auto empty_stream = std::make_shared<media_stream>("live/start-errors-empty");
    require(make_session(empty_stream, certificate)->start(*offer) == whep_session_start_error::stream_not_ready,
            "start errors stream without tracks");
}

void test_whep_session_lifecycle()
{
    boost::asio::io_context io;
    stream_registry registry;
    auto stream = std::make_shared<media_stream>("live/test");
    require(stream->update_track(make_video_track()), "whep video track");
    require(stream->update_track(make_audio_track()), "whep audio track");
    require(registry.add(stream), "whep registry add");

    whep_service whep(io, registry, boost::asio::ip::make_address("127.0.0.1"));
    require(whep.ready(), "whep certificate ready");

    auto empty_stream = std::make_shared<media_stream>("live/empty");
    require(registry.add(empty_stream), "whep empty stream add");
    require(whep.create("live/empty", webrtc_offer_sdp).error == whep_create_error::stream_not_ready, "whep empty stream not ready");

    auto missing_ice_offer = webrtc_offer_sdp;
    const std::string video_ice_ufrag = "a=ice-ufrag:remotevideo\r\n";
    const auto video_ice_offset = missing_ice_offer.find(video_ice_ufrag);
    require(video_ice_offset != std::string::npos, "whep invalid offer ice attribute");
    missing_ice_offer.erase(video_ice_offset, video_ice_ufrag.size());
    require(whep.create("live/test", missing_ice_offer).error == whep_create_error::invalid_offer, "whep semantic invalid offer");

    const auto first = whep.create("live/test", webrtc_offer_sdp);
    const auto second = whep.create("live/test", webrtc_offer_sdp);
    require(first.error == whep_create_error::none && second.error == whep_create_error::none, "whep create multiple sessions");
    require(!first.session_id.empty() && !second.session_id.empty(), "whep session ids");
    require(first.session_id != second.session_id, "whep unique session ids");
    require(first.answer_sdp.find("a=ice-lite\r\n") != std::string::npos, "whep answer sdp");
    require(first.answer_sdp.find("a=candidate:1 1 UDP 2130706431 127.0.0.1 ") != std::string::npos, "whep host candidate");
    require(sdp_attribute(first.answer_sdp, "ice-ufrag") != sdp_attribute(second.answer_sdp, "ice-ufrag"), "whep unique ice ufrag");
    require(sdp_attribute(first.answer_sdp, "ice-pwd") != sdp_attribute(second.answer_sdp, "ice-pwd"), "whep unique ice password");

    require(whep.remove(first.session_id), "whep remove first session");
    require(!whep.remove(first.session_id), "whep remove first once");
    require(whep.remove(second.session_id), "whep remove second session");

    const auto third = whep.create("live/test", webrtc_offer_sdp);
    require(third.error == whep_create_error::none, "whep recreate viewer");

    stream->end();
    require(!whep.remove(third.session_id), "whep source end releases session");
    registry.remove(*stream);

    auto replacement = std::make_shared<media_stream>("live/test");
    require(replacement->update_track(make_video_track()), "whep replacement video track");
    require(replacement->update_track(make_audio_track()), "whep replacement audio track");
    require(registry.add(replacement), "whep replacement registry add");

    const auto replacement_session = whep.create("live/test", webrtc_offer_sdp);
    require(replacement_session.error == whep_create_error::none, "whep create after republish");
    require(replacement_session.session_id != third.session_id, "whep republish new session id");

    auto updated_video = make_video_track();
    updated_video.codec_config.push_back(0x01);
    require(replacement->update_track(std::move(updated_video)), "whep source config update");
    require(!whep.remove(replacement_session.session_id), "whep source config change releases session");

    const auto updated_session = whep.create("live/test", webrtc_offer_sdp);
    require(updated_session.error == whep_create_error::none, "whep create after config change");
    require(whep.remove(updated_session.session_id), "whep remove updated session");
}

void test_whep_multi_session_isolation()
{
    boost::asio::io_context io;
    auto stream = std::make_shared<media_stream>("live/multi");
    require(stream->update_track(make_video_track()), "multi video track");
    require(stream->update_track(make_audio_track()), "multi audio track");

    const auto offer = parse_webrtc_offer(webrtc_offer_sdp);
    require(offer.has_value(), "multi parse offer");
    auto certificate = dtls_certificate::create();
    require(certificate != nullptr, "multi certificate");

    auto first = std::make_shared<whep_session>(io, stream, boost::asio::ip::make_address("127.0.0.1"), certificate);
    auto second = std::make_shared<whep_session>(io, stream, boost::asio::ip::make_address("127.0.0.1"), certificate);
    require(first->start(*offer) == whep_session_start_error::none && second->start(*offer) == whep_session_start_error::none,
            "multi sessions start");
    require(first->id() != second->id(), "multi unique session ids");
    require(first->local_port() != second->local_port(), "multi unique udp ports");

    const auto first_ufrag = sdp_attribute(first->answer_sdp(), "ice-ufrag");
    const auto first_pwd = sdp_attribute(first->answer_sdp(), "ice-pwd");
    const auto second_ufrag = sdp_attribute(second->answer_sdp(), "ice-ufrag");
    const auto second_pwd = sdp_attribute(second->answer_sdp(), "ice-pwd");
    require(first_ufrag != second_ufrag && first_pwd != second_pwd, "multi unique ice credentials");

    boost::asio::ip::udp::socket first_client(io, boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v4(), 0));
    boost::asio::ip::udp::socket second_client(io, boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v4(), 0));
    const boost::asio::ip::udp::endpoint first_endpoint(boost::asio::ip::make_address("127.0.0.1"), first->local_port());
    const boost::asio::ip::udp::endpoint second_endpoint(boost::asio::ip::make_address("127.0.0.1"), second->local_port());

    const std::array<std::uint8_t, 12> first_id{1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
    const std::array<std::uint8_t, 12> second_id{2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2};
    require_stun_success(exchange_stun(io, first_client, first_endpoint, make_stun_request(first_ufrag + ":remotevideo", first_pwd, first_id, true)),
                         first_id);
    require_stun_success(
        exchange_stun(io, second_client, second_endpoint, make_stun_request(second_ufrag + ":remotevideo", second_pwd, second_id, true)), second_id);
    require(first->ice_connected() && second->ice_connected(), "multi ice connected");

    first->close();
    require(first->local_port() == 0 && !first->ice_connected(), "multi first closed");
    require(second->local_port() != 0 && second->ice_connected(), "multi second remains connected");

    const std::array<std::uint8_t, 12> keepalive_id{3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3};
    require_stun_success(
        exchange_stun(io, second_client, second_endpoint, make_stun_request(second_ufrag + ":remotevideo", second_pwd, keepalive_id, false)),
        keepalive_id);
    require(second->ice_connected(), "multi second survives first close");

    stream->end();
    require(second->local_port() == 0, "multi source end closes second");
    require(!second->ice_connected(), "multi source end clears ice");

    boost::system::error_code error;
    first_client.close(error);
    second_client.close(error);
}

void test_whep_establishment_timeout()
{
    boost::asio::io_context io;
    auto stream = std::make_shared<media_stream>("live/establishment-timeout");
    require(stream->update_track(make_video_track()), "establishment timeout video track");
    require(stream->update_track(make_audio_track()), "establishment timeout audio track");

    const auto offer = parse_webrtc_offer(webrtc_offer_sdp);
    require(offer.has_value(), "establishment timeout parse offer");
    auto certificate = dtls_certificate::create();
    require(certificate != nullptr, "establishment timeout certificate");

    std::size_t close_count = 0;
    auto session = std::make_shared<whep_session>(
        io,
        stream,
        boost::asio::ip::make_address("127.0.0.1"),
        certificate,
        [&close_count](const whep_session&) { ++close_count; },
        whep_session_timeouts{
            .establishment = std::chrono::milliseconds(20),
            .ice_activity = std::chrono::seconds(1),
        });
    require(session->start(*offer) == whep_session_start_error::none, "establishment timeout session start");
    require(session->local_port() != 0, "establishment timeout socket open");

    io.run_for(std::chrono::milliseconds(80));
    io.restart();

    require(session->local_port() == 0, "establishment timeout closes socket");
    require(!session->ice_connected(), "establishment timeout clears ice");
    require(close_count == 1, "establishment timeout closes once");

    session->close();
    require(close_count == 1, "establishment timeout repeated close ignored");
}

void test_whep_ice_activity_timeout()
{
    boost::asio::io_context io;
    auto stream = std::make_shared<media_stream>("live/ice-activity-timeout");
    require(stream->update_track(make_video_track()), "ice activity timeout video track");
    require(stream->update_track(make_audio_track()), "ice activity timeout audio track");

    const auto offer = parse_webrtc_offer(webrtc_offer_sdp);
    require(offer.has_value(), "ice activity timeout parse offer");
    auto certificate = dtls_certificate::create();
    require(certificate != nullptr, "ice activity timeout certificate");

    std::size_t close_count = 0;
    auto session = std::make_shared<whep_session>(
        io,
        stream,
        boost::asio::ip::make_address("127.0.0.1"),
        certificate,
        [&close_count](const whep_session&) { ++close_count; },
        whep_session_timeouts{
            .establishment = std::chrono::seconds(1),
            .ice_activity = std::chrono::milliseconds(80),
        });
    require(session->start(*offer) == whep_session_start_error::none, "ice activity timeout session start");

    const auto local_ufrag = sdp_attribute(session->answer_sdp(), "ice-ufrag");
    const auto local_pwd = sdp_attribute(session->answer_sdp(), "ice-pwd");
    boost::asio::ip::udp::socket client(io, boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v4(), 0));
    const boost::asio::ip::udp::endpoint server_endpoint(boost::asio::ip::make_address("127.0.0.1"), session->local_port());

    const std::array<std::uint8_t, 12> nominate_id{4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4};
    require_stun_success(exchange_stun(io, client, server_endpoint, make_stun_request(local_ufrag + ":remotevideo", local_pwd, nominate_id, true)),
                         nominate_id);
    require(session->ice_connected(), "ice activity timeout nominated");

    io.run_for(std::chrono::milliseconds(50));
    io.restart();
    require(session->local_port() != 0, "ice activity timeout still active before refresh");

    const std::array<std::uint8_t, 12> refresh_id{5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5};
    require_stun_success(exchange_stun(io, client, server_endpoint, make_stun_request(local_ufrag + ":remotevideo", local_pwd, refresh_id, false)),
                         refresh_id);

    io.run_for(std::chrono::milliseconds(50));
    io.restart();
    require(session->local_port() != 0, "ice activity timeout refreshed by valid stun");

    io.run_for(std::chrono::milliseconds(70));
    io.restart();
    require(session->local_port() == 0, "ice activity timeout closes inactive session");
    require(!session->ice_connected(), "ice activity timeout clears ice");
    require(close_count == 1, "ice activity timeout closes once");

    boost::system::error_code error;
    client.close(error);
}

void test_whep_ice_lite()
{
    boost::asio::io_context io;
    auto stream = std::make_shared<media_stream>("live/ice");
    require(stream->update_track(make_video_track()), "ice video track");
    require(stream->update_track(make_audio_track()), "ice audio track");

    const auto offer = parse_webrtc_offer(webrtc_offer_sdp);
    require(offer.has_value(), "ice parse offer");
    auto certificate = dtls_certificate::create();
    require(certificate != nullptr, "ice certificate");

    auto session = std::make_shared<whep_session>(io, stream, boost::asio::ip::make_address("127.0.0.1"), certificate);
    require(session->start(*offer) == whep_session_start_error::none, "ice session start");

    const auto local_ufrag = sdp_attribute(session->answer_sdp(), "ice-ufrag");
    const auto local_pwd = sdp_attribute(session->answer_sdp(), "ice-pwd");
    require(!local_ufrag.empty() && !local_pwd.empty(), "ice local credentials");

    boost::asio::ip::udp::socket client(io, boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v4(), 0));
    const boost::asio::ip::udp::endpoint server_endpoint(boost::asio::ip::make_address("127.0.0.1"), session->local_port());
    const auto username = local_ufrag + ":remotevideo";

    const std::array<std::uint8_t, 12> check_id{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    const auto check_response = exchange_stun(io, client, server_endpoint, make_stun_request(username, local_pwd, check_id, false));
    require_stun_success(check_response, check_id);
    require(!session->ice_connected(), "ice check not nominated");

    const std::array<std::uint8_t, 12> nominate_id{11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0};
    const auto nominate_response = exchange_stun(io, client, server_endpoint, make_stun_request(username, local_pwd, nominate_id, true));
    require_stun_success(nominate_response, nominate_id);
    require(session->ice_connected(), "ice nominated");

    session->close();
    boost::system::error_code error;
    client.close(error);
}

void test_whep_selected_bundle_transport()
{
    boost::asio::io_context io;
    auto certificate = dtls_certificate::create();
    require(certificate != nullptr, "selected transport certificate");
    const auto check = [&io, &certificate](media_track track, const std::string& sdp, std::string_view remote_ufrag, std::uint8_t id)
    {
        auto stream = std::make_shared<media_stream>("live/selected-transport");
        require(stream->update_track(std::move(track)), "selected transport track");
        const auto offer = parse_webrtc_offer(sdp);
        require(offer.has_value(), "selected transport offer");
        auto session = std::make_shared<whep_session>(io, stream, boost::asio::ip::make_address("127.0.0.1"), certificate);
        require(session->start(*offer) == whep_session_start_error::none, "selected transport session start");

        const auto local_ufrag = sdp_attribute(session->answer_sdp(), "ice-ufrag");
        const auto local_pwd = sdp_attribute(session->answer_sdp(), "ice-pwd");
        boost::asio::ip::udp::socket client(io, boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v4(), 0));
        const boost::asio::ip::udp::endpoint endpoint(boost::asio::ip::make_address("127.0.0.1"), session->local_port());
        std::array<std::uint8_t, 12> transaction_id{};
        transaction_id.fill(id);
        require_stun_success(
            exchange_stun(io, client, endpoint, make_stun_request(local_ufrag + ":" + std::string(remote_ufrag), local_pwd, transaction_id, true)),
            transaction_id);
        session->close();
    };

    check(make_audio_track(), webrtc_offer_sdp, "remoteaudio", 6);
    auto video_tag_sdp = webrtc_offer_sdp;
    constexpr std::string_view bundle_group = "a=group:BUNDLE 0 1";
    const auto bundle_offset = video_tag_sdp.find(bundle_group);
    require(bundle_offset != std::string::npos, "selected transport bundle source");
    video_tag_sdp.replace(bundle_offset, bundle_group.size(), "a=group:BUNDLE 1 0");
    check(make_video_track(), video_tag_sdp, "remotevideo", 7);
    constexpr std::string_view h264_encoding = "H264/90000";
    const auto encoding_offset = video_tag_sdp.find(h264_encoding);
    require(encoding_offset != std::string::npos, "selected transport h265 source");
    video_tag_sdp.replace(encoding_offset, h264_encoding.size(), "H265/90000");
    check(make_h265_track(), video_tag_sdp, "remotevideo", 8);
}

void test_rtcp_receiver()
{
    rtcp_receiver receiver;
    rtcp_receive_result result;

    const auto compound = make_rtcp_compound();
    require(receiver.input(compound, result), "rtcp compound parse");
    require(result.receiver_reports.size() == 2U, "rtcp receiver report count");
    require(result.plis.size() == 1U, "rtcp pli count");

    const auto& first = result.receiver_reports[0];
    require(first.sender_ssrc == 0x0102'0304U, "rtcp rr sender ssrc");
    require(first.source_ssrc == 0x1112'1314U, "rtcp rr source ssrc");
    require(first.fraction_lost == 7U, "rtcp rr fraction lost");
    require(first.cumulative_lost == -2, "rtcp rr cumulative lost");
    require(first.highest_sequence == 0x2122'2324U, "rtcp rr highest sequence");
    require(first.jitter == 0x3132'3334U, "rtcp rr jitter");
    require(first.lsr == 0x4142'4344U, "rtcp rr lsr");
    require(first.dlsr == 0x5152'5354U, "rtcp rr dlsr");

    const auto& pli = result.plis.front();
    require(pli.sender_ssrc == 0x0102'0304U, "rtcp pli sender ssrc");
    require(pli.media_ssrc == 0x1112'1314U, "rtcp pli media ssrc");

    const auto nack = make_rtcp_nack();
    require(receiver.input(nack, result), "rtcp nack parse");
    require(result.receiver_reports.empty() && result.plis.empty(), "rtcp nack ignored");

    auto invalid_length = make_rtcp_receiver_report();
    invalid_length[3] = 14;
    require(!receiver.input(invalid_length, result), "rtcp invalid outer length");

    const std::array<std::uint8_t, 3> truncated{0x80, 201, 0};
    require(!receiver.input(truncated, result), "rtcp truncated header");

    const std::array<std::uint8_t, 8> unknown{0x80, 208, 0, 1, 0, 0, 0, 1};
    require(!receiver.input(unknown, result), "rtcp unknown packet type");

    auto invalid_pli = make_rtcp_pli();
    invalid_pli[3] = 3;
    invalid_pli.insert(invalid_pli.end(), 4U, 0);
    require(!receiver.input(invalid_pli, result), "rtcp invalid pli length");
}

media_frame make_audio_frame(std::size_t index, std::int64_t pts_ns)
{
    return media_frame{
        .track = audio_track_id,
        .dts_ns = pts_ns,
        .pts_ns = pts_ns,
        .key_frame = false,
        .payload = std::make_shared<const std::vector<std::uint8_t>>(valid_aac_adts_frames.at(index)),
    };
}

void test_whep_dtls(codec_id video_codec)
{
    require(video_codec == codec_id::h264 || video_codec == codec_id::h265, "dtls video codec");
    const bool h265 = video_codec == codec_id::h265;
    boost::asio::io_context io;
    auto stream = std::make_shared<media_stream>("live/dtls");
    require(stream->update_track(h265 ? make_h265_track() : make_video_track()), "dtls video track");
    require(stream->update_track(make_audio_track()), "dtls audio track");

    auto server_certificate = dtls_certificate::create();
    auto client_certificate = dtls_certificate::create();
    require(server_certificate != nullptr && client_certificate != nullptr, "dtls certificates");

    auto offer_sdp = offer_with_fingerprint(client_certificate->sha256_fingerprint());
    if (h265)
    {
        offer_sdp = make_h265_offer(std::move(offer_sdp));
    }
    const auto offer = parse_webrtc_offer(offer_sdp);
    require(offer.has_value(), "dtls parse offer");

    auto session = std::make_shared<whep_session>(io, stream, boost::asio::ip::make_address("127.0.0.1"), server_certificate);
    require(session->start(*offer) == whep_session_start_error::none, "dtls session start");

    const auto local_ufrag = sdp_attribute(session->answer_sdp(), "ice-ufrag");
    const auto local_pwd = sdp_attribute(session->answer_sdp(), "ice-pwd");
    require(!local_ufrag.empty() && !local_pwd.empty(), "dtls ice credentials");

    boost::asio::ip::udp::socket client_socket(io, boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v4(), 0));
    const boost::asio::ip::udp::endpoint server_endpoint(boost::asio::ip::make_address("127.0.0.1"), session->local_port());
    const std::array<std::uint8_t, 12> nominate_id{9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 1, 2};
    const auto nominate_response =
        exchange_stun(io, client_socket, server_endpoint, make_stun_request(local_ufrag + ":remotevideo", local_pwd, nominate_id, true));
    require_stun_success(nominate_response, nominate_id);
    require(session->ice_connected(), "dtls ice connected");

    auto client = make_dtls_test_client(client_certificate);
    require(client.has_value(), "dtls client create");
    require(drive_dtls_client(io, client_socket, server_endpoint, *session, *client), "dtls handshake");
    require(session->dtls_connected(), "dtls server connected");

    require(session->srtp_started(), "srtp server started");

    const auto peer_material = make_peer_srtp_material(client->ssl.get());
    require(peer_material.has_value(), "dtls client srtp material");
    srtp_transport peer_srtp;
    require(peer_srtp.start(*peer_material), "srtp peer start");
    require(stream->publish(make_video_key_frame(video_codec)), "srtp publish video");

    std::array<std::uint8_t, 4096> rtp_buffer{};
    boost::asio::ip::udp::endpoint rtp_sender;
    std::optional<srtp_packet> clear_rtp;
    const auto rtp_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!clear_rtp && std::chrono::steady_clock::now() < rtp_deadline)
    {
        io.run_for(std::chrono::milliseconds(10));
        io.restart();

        boost::system::error_code receive_error;
        const auto size = client_socket.receive_from(boost::asio::buffer(rtp_buffer), rtp_sender, 0, receive_error);
        if (receive_error == boost::asio::error::would_block || receive_error == boost::asio::error::try_again)
        {
            continue;
        }
        require(!receive_error && rtp_sender == server_endpoint, "srtp receive packet");
        const auto packet = std::span<const std::uint8_t>(rtp_buffer.data(), size);
        if (!srtp_transport::is_rtp_or_rtcp(packet))
        {
            continue;
        }
        clear_rtp = peer_srtp.unprotect(packet);
    }

    require(clear_rtp.has_value() && !clear_rtp->rtcp, "srtp decrypt video rtp");
    require(clear_rtp->bytes.size() >= 14U, "srtp clear rtp header");
    require((clear_rtp->bytes[0] >> 6U) == 2U, "srtp clear rtp version");
    require((clear_rtp->bytes[1] & 0x7fU) == 102U, "srtp negotiated video payload");
    const auto nal_type = h265 ? ((clear_rtp->bytes[12] >> 1U) & 0x3fU) : (clear_rtp->bytes[12] & 0x1fU);
    require(nal_type == (h265 ? 19U : 5U), "srtp negotiated video codec");
    const auto video_ssrc = read_network_u32(clear_rtp->bytes, 8U);

    std::optional<srtp_packet> clear_server_rtcp;
    const auto server_rtcp_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!clear_server_rtcp && std::chrono::steady_clock::now() < server_rtcp_deadline)
    {
        io.run_for(std::chrono::milliseconds(10));
        io.restart();

        boost::system::error_code receive_error;
        const auto size = client_socket.receive_from(boost::asio::buffer(rtp_buffer), rtp_sender, 0, receive_error);
        if (receive_error == boost::asio::error::would_block || receive_error == boost::asio::error::try_again)
        {
            continue;
        }
        require(!receive_error && rtp_sender == server_endpoint, "srtcp receive server report");
        const auto packet = std::span<const std::uint8_t>(rtp_buffer.data(), size);
        if (!srtp_transport::is_rtp_or_rtcp(packet))
        {
            continue;
        }
        auto clear = peer_srtp.unprotect(packet);
        if (clear && clear->rtcp)
        {
            clear_server_rtcp = std::move(clear);
        }
    }

    require(clear_server_rtcp.has_value(), "srtcp decrypt server sender report");
    require_server_sender_report(clear_server_rtcp->bytes, video_ssrc, session->id());

    std::int64_t audio_pts_ns = 0;
    for (std::size_t index = 0; index < valid_aac_adts_frames.size(); ++index)
    {
        require(stream->publish(make_audio_frame(index, audio_pts_ns)), "srtp publish audio");
        audio_pts_ns += 23'219'954;
    }

    std::optional<srtp_packet> clear_audio_rtp;
    const auto audio_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!clear_audio_rtp && std::chrono::steady_clock::now() < audio_deadline)
    {
        io.run_for(std::chrono::milliseconds(10));
        io.restart();

        boost::system::error_code receive_error;
        const auto size = client_socket.receive_from(boost::asio::buffer(rtp_buffer), rtp_sender, 0, receive_error);
        if (receive_error == boost::asio::error::would_block || receive_error == boost::asio::error::try_again)
        {
            continue;
        }
        require(!receive_error && rtp_sender == server_endpoint, "srtp receive audio packet");
        const auto packet = std::span<const std::uint8_t>(rtp_buffer.data(), size);
        if (!srtp_transport::is_rtp_or_rtcp(packet))
        {
            continue;
        }
        auto clear = peer_srtp.unprotect(packet);
        if (clear && !clear->rtcp && clear->bytes.size() >= 12U && (clear->bytes[1] & 0x7fU) == 111U)
        {
            clear_audio_rtp = std::move(clear);
        }
    }

    require(clear_audio_rtp.has_value(), "srtp decrypt opus rtp");
    require((clear_audio_rtp->bytes[0] >> 6U) == 2U, "srtp clear opus rtp version");
    require((clear_audio_rtp->bytes[1] & 0x7fU) == 111U, "srtp negotiated opus payload");

    const auto clear_rtcp = make_rtcp_compound();
    const auto protected_rtcp = peer_srtp.protect_rtcp(clear_rtcp);
    require(protected_rtcp.has_value(), "srtcp peer protect");
    static_cast<void>(client_socket.send_to(boost::asio::buffer(*protected_rtcp), server_endpoint));
    io.run_for(std::chrono::milliseconds(50));
    io.restart();
    require(session->rtcp_stats().receiver_reports == 2U, "srtcp receiver report dispatch");
    require(session->rtcp_stats().plis == 1U, "srtcp pli dispatch");

    session->close();
    boost::system::error_code error;
    client_socket.close(error);
}

}    // namespace
}    // namespace media_server

int main()
{
    media_server::test_webrtc_sdp_answer();
    std::cout << "[pass] webrtc_sdp_answer\n";
    media_server::test_webrtc_h265_sdp_answer();
    std::cout << "[pass] webrtc_h265_sdp_answer\n";
    media_server::test_webrtc_payload_type_membership();
    std::cout << "[pass] webrtc_payload_type_membership\n";
    media_server::test_webrtc_payload_type_range();
    std::cout << "[pass] webrtc_payload_type_range\n";
    media_server::test_webrtc_disabled_media();
    std::cout << "[pass] webrtc_disabled_media\n";
    media_server::test_webrtc_single_media_per_kind();
    std::cout << "[pass] webrtc_single_media_per_kind\n";
    media_server::test_webrtc_opus_mono_default();
    std::cout << "[pass] webrtc_opus_mono_default\n";
    media_server::test_webrtc_transport_contract();
    std::cout << "[pass] webrtc_transport_contract\n";
    media_server::test_whep_session_start_errors();
    std::cout << "[pass] whep_session_start_errors\n";
    media_server::test_whep_session_lifecycle();
    std::cout << "[pass] whep_session_lifecycle\n";
    media_server::test_whep_http_cors();
    std::cout << "[pass] whep_http_cors\n";
    media_server::test_whep_multi_session_isolation();
    std::cout << "[pass] whep_multi_session_isolation\n";
    media_server::test_whep_establishment_timeout();
    std::cout << "[pass] whep_establishment_timeout\n";
    media_server::test_whep_ice_activity_timeout();
    std::cout << "[pass] whep_ice_activity_timeout\n";
    media_server::test_whep_ice_lite();
    std::cout << "[pass] whep_ice_lite\n";
    media_server::test_whep_selected_bundle_transport();
    std::cout << "[pass] whep_selected_bundle_transport\n";
    media_server::test_rtcp_receiver();
    std::cout << "[pass] rtcp_receiver\n";
    media_server::test_whep_dtls(media_server::codec_id::h264);
    std::cout << "[pass] whep_dtls_h264\n";
    media_server::test_whep_dtls(media_server::codec_id::h265);
    std::cout << "[pass] whep_dtls_h265\n";
    std::cout << "all tests passed\n";
    return 0;
}
