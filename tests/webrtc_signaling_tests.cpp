#include <span>
#include <array>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <climits>
#include <cstdlib>
#include <utility>
#include <iostream>
#include <string_view>

#include <srtp2/srtp.h>
#include <boost/crc.hpp>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/hmac.h>
#include <openssl/srtp.h>
#include <boost/asio/write.hpp>
#include <boost/beast/http.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/asio/executor_work_guard.hpp>

#include "media/hls/hls.h"
#include "media/core/media_stream.h"
#include "media/http/http_session.h"
#include "media/webrtc/webrtc_sdp.h"
#include "media/webrtc/stun_message.h"
#include "media/webrtc/whep.h"
#include "media/webrtc/whep_session.h"
#include "media/core/stream_registry.h"
#include "media/webrtc/srtp_transport.h"
#include "media/gb28181/gb28181.h"
#include "media/net/io_context_pool.h"
#include "media/webrtc/dtls_certificate.h"

extern "C"
{
#include "rtp-ext.h"
#include "rtp-packet.h"
#include "rtp-profile.h"
}

namespace media_server
{
namespace
{

constexpr track_id video_track_id = 1;
constexpr track_id audio_track_id = 2;

struct test_srtp_packet
{
    bool rtcp{};
    std::vector<std::uint8_t> bytes;
};

struct test_srtp_keying_material
{
    dtls_srtp_keying_material outbound;
    std::vector<std::uint8_t> inbound_key;
    std::vector<std::uint8_t> inbound_salt;
};

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
    "a=extmap:4 urn:ietf:params:rtp-hdrext:sdes:mid\r\n"
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
    "a=extmap:4 urn:ietf:params:rtp-hdrext:sdes:mid\r\n"
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

std::optional<dtls_test_client> make_dtls_test_client(const std::shared_ptr<dtls_certificate>& certificate, const char* srtp_profile)
{
    dtls_test_client client{
        .context = ssl_context_ptr(SSL_CTX_new(DTLS_method())),
        .ssl = {},
        .read_bio = nullptr,
        .write_bio = nullptr,
    };
    if (!client.context || SSL_CTX_set_min_proto_version(client.context.get(), DTLS1_2_VERSION) != 1 ||
        SSL_CTX_set_max_proto_version(client.context.get(), DTLS1_2_VERSION) != 1 ||
        SSL_CTX_set_tlsext_use_srtp(client.context.get(), srtp_profile) != 0 ||
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

std::optional<test_srtp_keying_material> make_peer_srtp_material(SSL* ssl)
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
    else if (name == "SRTP_AEAD_AES_256_GCM")
    {
        key_size = 32;
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
    return test_srtp_keying_material{
        .outbound =
            dtls_srtp_keying_material{
                .profile = std::string(name),
                .server_write_key = std::vector<std::uint8_t>(client_key, server_key),
                .server_write_salt = std::vector<std::uint8_t>(client_salt, server_salt),
            },
        .inbound_key = std::vector<std::uint8_t>(server_key, client_salt),
        .inbound_salt = std::vector<std::uint8_t>(server_salt, material.end()),
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
    offer.replace(fmtp_offset, h264_fmtp.size(), "a=fmtp:102 profile-space=0;profile-id=1;tier-flag=0;level-id=120\r\n");
    offer.replace(rtpmap_offset, h264_rtpmap.size(), "a=rtpmap:102 H265/90000\r\n");
    return offer;
}

std::string make_audio_tag_offer(std::string offer)
{
    constexpr std::string_view bundle = "a=group:BUNDLE 0 1\r\n";
    const auto offset = offer.find(bundle);
    require(offset != std::string::npos, "webrtc audio bundle tag source");
    offer.replace(offset, bundle.size(), "a=group:BUNDLE 1 0\r\n");
    return offer;
}

std::string make_av1_offer(std::string offer, int payload_type, int rtx_payload_type, std::string_view format_parameters)
{
    constexpr std::string_view video_mline = "m=video 9 UDP/TLS/RTP/SAVPF 102 127\r\n";
    constexpr std::string_view h264_rtpmap = "a=rtpmap:102 H264/90000\r\n";
    constexpr std::string_view h264_fmtp = "a=fmtp:102 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f\r\n";
    constexpr std::string_view rtx_rtpmap = "a=rtpmap:127 rtx/90000\r\n";
    constexpr std::string_view rtx_fmtp = "a=fmtp:127 apt=102\r\n";
    const auto mline_offset = offer.find(video_mline);
    const auto rtpmap_offset = offer.find(h264_rtpmap);
    const auto fmtp_offset = offer.find(h264_fmtp);
    const auto rtx_rtpmap_offset = offer.find(rtx_rtpmap);
    const auto rtx_fmtp_offset = offer.find(rtx_fmtp);
    require(mline_offset != std::string::npos && rtpmap_offset != std::string::npos && fmtp_offset != std::string::npos &&
                rtx_rtpmap_offset != std::string::npos && rtx_fmtp_offset != std::string::npos,
            "webrtc av1 offer source");
    const auto payload = std::to_string(payload_type);
    const auto rtx_payload = std::to_string(rtx_payload_type);
    offer.replace(rtx_fmtp_offset, rtx_fmtp.size(), "a=fmtp:" + rtx_payload + " apt=" + payload + "\r\n");
    offer.replace(rtx_rtpmap_offset, rtx_rtpmap.size(), "a=rtpmap:" + rtx_payload + " rtx/90000\r\n");
    offer.replace(fmtp_offset,
                  h264_fmtp.size(),
                  format_parameters.empty() ? std::string{} : "a=fmtp:" + payload + ' ' + std::string(format_parameters) + "\r\n");
    offer.replace(rtpmap_offset, h264_rtpmap.size(), "a=rtpmap:" + payload + " AV1/90000\r\n");
    offer.replace(mline_offset, video_mline.size(), "m=video 9 UDP/TLS/RTP/SAVPF " + payload + ' ' + rtx_payload + "\r\n");
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

std::span<const std::uint8_t> require_rtp_mid(std::span<const std::uint8_t> packet, std::string_view expected_mid, int expected_id)
{
    rtp_packet_t parsed{};
    require(rtp_packet_deserialize(&parsed, packet.data(), static_cast<int>(packet.size())) == 0, "srtp mid deserialize");
    require(parsed.rtp.x == 1 && parsed.extension != nullptr && parsed.extlen > 0, "srtp mid extension present");

    const auto extension = std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(parsed.extension), static_cast<std::size_t>(parsed.extlen));
    std::size_t offset = 0;
    std::size_t length = 0;
    int extension_id = 0;
    if (parsed.extprofile == RTP_HDREXT_PROFILE_ONE_BYTE)
    {
        require(!extension.empty(), "srtp mid one byte header");
        extension_id = extension[0] >> 4U;
        length = (extension[0] & 0x0fU) + 1U;
        offset = 1;
    }
    else
    {
        require((parsed.extprofile & RTP_HDREXT_PROFILE_TWO_BYTE_FILTER) == RTP_HDREXT_PROFILE_TWO_BYTE && extension.size() >= 2U,
                "srtp mid two byte header");
        extension_id = extension[0];
        length = extension[1];
        offset = 2;
    }
    require(extension_id == expected_id && offset + length <= extension.size(), "srtp mid id and length");
    const auto mid = std::string_view(reinterpret_cast<const char*>(extension.data() + offset), length);
    require(mid == expected_mid, "srtp mid value");
    return std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(parsed.payload), static_cast<std::size_t>(parsed.payloadlen));
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

enum class stun_request_variant
{
    valid,
    missing_priority,
    missing_ice_controlling,
    ice_controlled,
    missing_fingerprint,
    use_candidate_after_integrity,
    attributes_after_integrity,
};

std::vector<std::uint8_t> make_stun_request(std::string_view username,
                                            std::string_view password,
                                            const std::array<std::uint8_t, 12>& transaction_id,
                                            bool use_candidate,
                                            stun_request_variant variant = stun_request_variant::valid,
                                            std::span<const std::uint16_t> extra_attributes = {})
{
    std::vector<std::uint8_t> packet;
    append_u16(packet, 0x0001);
    append_u16(packet, 0);
    append_u32(packet, stun_magic_cookie);
    packet.insert(packet.end(), transaction_id.begin(), transaction_id.end());

    append_stun_attribute(packet, 0x0006, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(username.data()), username.size()));
    if (variant != stun_request_variant::missing_priority)
    {
        constexpr std::array<std::uint8_t, 4> priority{0x6e, 0x7f, 0xff, 0xff};
        append_stun_attribute(packet, 0x0024, priority);
    }
    if (variant != stun_request_variant::missing_ice_controlling)
    {
        constexpr std::array<std::uint8_t, 8> tie_breaker{0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
        append_stun_attribute(packet, variant == stun_request_variant::ice_controlled ? 0x8029 : 0x802a, tie_breaker);
    }
    if (use_candidate && variant != stun_request_variant::use_candidate_after_integrity)
    {
        append_stun_attribute(packet, 0x0025, {});
    }
    if (variant != stun_request_variant::attributes_after_integrity)
    {
        for (const auto type : extra_attributes)
        {
            append_stun_attribute(packet, type, {});
        }
    }

    set_stun_length(packet, packet.size() - 20U + 24U);
    const auto digest = stun_hmac(password, packet);
    append_stun_attribute(packet, 0x0008, digest);

    if (use_candidate && variant == stun_request_variant::use_candidate_after_integrity)
    {
        append_stun_attribute(packet, 0x0025, {});
    }
    if (variant == stun_request_variant::attributes_after_integrity)
    {
        for (const auto type : extra_attributes)
        {
            append_stun_attribute(packet, type, {});
        }
    }

    if (variant != stun_request_variant::missing_fingerprint)
    {
        set_stun_length(packet, packet.size() - 20U + 8U);
        const auto fingerprint = stun_fingerprint(packet);
        const std::array<std::uint8_t, 4> fingerprint_bytes{
            static_cast<std::uint8_t>(fingerprint >> 24U),
            static_cast<std::uint8_t>((fingerprint >> 16U) & 0xffU),
            static_cast<std::uint8_t>((fingerprint >> 8U) & 0xffU),
            static_cast<std::uint8_t>(fingerprint & 0xffU),
        };
        append_stun_attribute(packet, 0x8028, fingerprint_bytes);
    }
    else
    {
        set_stun_length(packet, packet.size() - 20U);
    }
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

void drain_io(boost::asio::io_context& io)
{
    io.poll();
    io.restart();
}

void require_stun_success(std::span<const std::uint8_t> response, const std::array<std::uint8_t, 12>& transaction_id)
{
    require(response.size() >= 20U, "stun response size");
    require(response[0] == 0x01 && response[1] == 0x01, "stun binding success type");
    require(response[4] == 0x21 && response[5] == 0x12 && response[6] == 0xa4 && response[7] == 0x42, "stun response cookie");
    require(std::equal(transaction_id.begin(), transaction_id.end(), response.begin() + 8), "stun response transaction id");
}

void require_stun_error(std::span<const std::uint8_t> response,
                        const std::array<std::uint8_t, 12>& transaction_id,
                        int expected_code,
                        std::span<const std::uint16_t> expected_unknown_attributes,
                        std::string_view password)
{
    require(response.size() >= 20U, "stun error response size");
    require(response[0] == 0x01 && response[1] == 0x11, "stun binding error type");
    require(response[4] == 0x21 && response[5] == 0x12 && response[6] == 0xa4 && response[7] == 0x42, "stun error response cookie");
    require(std::equal(transaction_id.begin(), transaction_id.end(), response.begin() + 8), "stun error response transaction id");
    require(read_network_u16(response, 2U) == response.size() - 20U, "stun error response length");

    int error_code = 0;
    std::vector<std::uint16_t> unknown_attributes;
    std::optional<std::size_t> message_integrity_offset;
    std::optional<std::size_t> fingerprint_offset;
    std::size_t offset = 20U;
    while (offset + 4U <= response.size())
    {
        const auto type = read_network_u16(response, offset);
        const auto length = static_cast<std::size_t>(read_network_u16(response, offset + 2U));
        const auto value_offset = offset + 4U;
        require(value_offset + length <= response.size(), "stun error attribute range");

        if (type == 0x0009)
        {
            require(length >= 4U, "stun error code length");
            error_code = static_cast<int>(response[value_offset + 2U] & 0x07U) * 100 + static_cast<int>(response[value_offset + 3U]);
        }
        else if (type == 0x000a)
        {
            require((length % 2U) == 0U, "stun unknown attributes length");
            for (std::size_t index = 0; index < length; index += 2U)
            {
                unknown_attributes.push_back(read_network_u16(response, value_offset + index));
            }
        }
        else if (type == 0x0008)
        {
            require(length == 20U && !message_integrity_offset.has_value(), "stun error message integrity");
            message_integrity_offset = offset;
        }
        else if (type == 0x8028)
        {
            require(length == 4U && !fingerprint_offset.has_value(), "stun error fingerprint");
            fingerprint_offset = offset;
        }

        offset = value_offset + length;
        offset += (4U - (offset % 4U)) % 4U;
    }

    require(offset == response.size(), "stun error response attributes");
    require(error_code == expected_code, "stun error response code");
    require(unknown_attributes.size() == expected_unknown_attributes.size() &&
                std::equal(unknown_attributes.begin(), unknown_attributes.end(), expected_unknown_attributes.begin()),
            "stun error unknown attributes");
    require(message_integrity_offset.has_value(), "stun error message integrity present");
    require(fingerprint_offset.has_value() && *fingerprint_offset + 8U == response.size(), "stun error fingerprint present");

    std::vector<std::uint8_t> integrity_input(response.begin(), response.begin() + static_cast<std::ptrdiff_t>(*message_integrity_offset));
    set_stun_length(integrity_input, *message_integrity_offset - 20U + 24U);
    const auto digest = stun_hmac(password, integrity_input);
    require(std::equal(digest.begin(), digest.end(), response.begin() + static_cast<std::ptrdiff_t>(*message_integrity_offset + 4U)),
            "stun error message integrity valid");
    require(stun_fingerprint(response.first(*fingerprint_offset)) == read_network_u32(response, *fingerprint_offset + 4U),
            "stun error fingerprint valid");
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

media_track make_opus_track(std::uint16_t channel_count = 2)
{
    return media_track{
        .id = audio_track_id,
        .kind = media_kind::audio,
        .codec = codec_id::opus,
        .clock_rate = 48'000,
        .channel_count = channel_count,
        .codec_config = {},
    };
}

media_track make_g711_track(codec_id codec)
{
    require(codec == codec_id::g711a || codec == codec_id::g711u, "webrtc g711 track codec");
    return media_track{
        .id = audio_track_id,
        .kind = media_kind::audio,
        .codec = codec,
        .clock_rate = 8'000,
        .channel_count = 1,
        .codec_config = {},
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
          workers_(1),
          acceptor_(io_, {boost::asio::ip::tcp::v4(), 0})
    {
        streams_.clear();
        stream_ = std::make_shared<media_stream>("live/camera", io_.get_executor());
        require(stream_->set_tracks({make_video_track(), make_audio_track()}), "initial tracks");
        require(streams_.add(stream_), "whep http registry add");
        runner_ = std::jthread([this]() { io_.run(); });
    }

    ~whep_http_test_peer()
    {
        boost::asio::post(io_,
                          [this]()
                          {
                              work_.reset();
        });
        runner_.join();
        hls::shutdown();
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

    boost::beast::http::response<boost::beast::http::string_body> request(boost::beast::http::verb method, std::string target)
    {
        boost::beast::http::request<boost::beast::http::string_body> request{method, std::move(target), 11};
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
        auto session = std::make_shared<http_session>(std::move(server_socket), workers_, config_);
        session->startup();

        const bool head = request.method() == boost::beast::http::verb::head;
        boost::beast::http::write(client, request);
        boost::beast::flat_buffer buffer;
        boost::beast::http::response<boost::beast::http::string_body> response;
        if (head)
        {
            boost::beast::http::response_parser<boost::beast::http::string_body> parser;
            parser.skip(true);
            boost::beast::http::read(client, buffer, parser);
            response = parser.release();

            std::array<std::uint8_t, 256> trailing{};
            std::size_t trailing_bytes = buffer.size();
            boost::system::error_code read_error;
            while (!read_error)
            {
                trailing_bytes += client.read_some(boost::asio::buffer(trailing), read_error);
            }
            require(read_error == boost::asio::error::eof, "http head response close");
            require(trailing_bytes == 0, "http head response wire body");
        }
        else
        {
            boost::beast::http::read(client, buffer, response);
        }
        boost::system::error_code error;
        client.close(error);
        return response;
    }

    boost::asio::io_context io_;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work_;
    config config_;
    stream_registry& streams_ = registry::instance();
    io_context_pool workers_;
    boost::asio::ip::tcp::acceptor acceptor_;
    boost::asio::io_context client_io_;
    std::shared_ptr<media_stream> stream_;
    std::jthread runner_;
};

void require_whep_options(const boost::beast::http::response<boost::beast::http::string_body>& response, std::string_view methods, bool accept_post)
{
    require(response.result() == boost::beast::http::status::ok, "whep options status");
    require(response["Access-Control-Allow-Origin"] == "*", "whep options allow origin");
    require(response["Access-Control-Allow-Methods"] == methods, "whep options allow methods");
    require(response["Access-Control-Allow-Headers"] == "Content-Type", "whep options allow headers");
    require((response["Accept-Post"] == "application/sdp") == accept_post, "whep options accept post");
    require(response[boost::beast::http::field::content_length] == "0", "whep options content length");
    require(response.body().empty(), "whep options empty body");
}

void test_http_head_response_contract()
{
    whep_http_test_peer peer;

    const auto text_get = peer.request(boost::beast::http::verb::get, "/");
    require(text_get.result() == boost::beast::http::status::not_found && text_get.body() == "not found\n", "http text get error");
    const auto text_head = peer.request(boost::beast::http::verb::head, "/");
    require(text_head.result() == text_get.result(), "http text head status");
    require(text_head[boost::beast::http::field::content_type] == text_get[boost::beast::http::field::content_type], "http text head content type");
    require(text_head[boost::beast::http::field::content_length] == text_get[boost::beast::http::field::content_length],
            "http text head content length");
    require(text_head.body().empty(), "http text head body");

    const auto whep_get = peer.request(boost::beast::http::verb::get, "/whep");
    require(whep_get.result() == boost::beast::http::status::not_found && whep_get.body() == "not found\n", "whep invalid resource get");
    const auto whep_head = peer.request(boost::beast::http::verb::head, "/whep");
    require(whep_head.result() == whep_get.result(), "whep invalid resource head status");
    require(whep_head[boost::beast::http::field::content_type] == whep_get[boost::beast::http::field::content_type],
            "whep invalid resource head content type");
    require(whep_head[boost::beast::http::field::content_length] == whep_get[boost::beast::http::field::content_length],
            "whep invalid resource head content length");
    require(whep_head[boost::beast::http::field::access_control_allow_origin] == whep_get[boost::beast::http::field::access_control_allow_origin],
            "whep invalid resource head allow origin");
    require(whep_head.body().empty(), "whep invalid resource head body");
}

void test_http_method_contract()
{
    whep_http_test_peer peer;

    const auto hls_post = peer.request(boost::beast::http::verb::post, "/hls/live/camera/index.m3u8");
    require(hls_post.result() == boost::beast::http::status::method_not_allowed, "http hls post status");
    require(hls_post[boost::beast::http::field::allow] == "GET", "http hls post allow");

    const auto flv_post = peer.request(boost::beast::http::verb::post, "/live/camera.flv");
    require(flv_post.result() == boost::beast::http::status::method_not_allowed, "http flv post status");
    require(flv_post[boost::beast::http::field::allow] == "GET", "http flv post allow");

    const auto hls_head = peer.request(boost::beast::http::verb::head, "/hls/live/camera/index.m3u8");
    require(hls_head.result() == boost::beast::http::status::method_not_allowed, "http hls head status");
    require(hls_head[boost::beast::http::field::allow] == "GET", "http hls head allow");
    require(hls_head.body().empty(), "http hls head body");
}

void test_whep_http_cors()
{
    whep_http_test_peer peer;
    require_whep_options(peer.options("/whep/live/camera", "POST"), "GET, HEAD, POST, OPTIONS", true);

    const auto endpoint_get = peer.request(boost::beast::http::verb::get, "/whep/live/camera");
    require(endpoint_get.result() == boost::beast::http::status::ok, "whep endpoint get status");
    require(endpoint_get[boost::beast::http::field::content_type] == "application/sdp", "whep endpoint get content type");
    require(endpoint_get[boost::beast::http::field::content_length] == "0", "whep endpoint get content length");
    require(endpoint_get.body().empty(), "whep endpoint get body");

    const auto endpoint_head = peer.request(boost::beast::http::verb::head, "/whep/live/camera");
    require(endpoint_head.result() == endpoint_get.result(), "whep endpoint head status");
    require(endpoint_head[boost::beast::http::field::content_type] == endpoint_get[boost::beast::http::field::content_type],
            "whep endpoint head content type");
    require(endpoint_head[boost::beast::http::field::content_length] == endpoint_get[boost::beast::http::field::content_length],
            "whep endpoint head content length");
    require(endpoint_head[boost::beast::http::field::cache_control] == endpoint_get[boost::beast::http::field::cache_control],
            "whep endpoint head cache control");
    require(
        endpoint_head[boost::beast::http::field::access_control_allow_origin] == endpoint_get[boost::beast::http::field::access_control_allow_origin],
        "whep endpoint head allow origin");
    require(endpoint_head.body().empty(), "whep endpoint head body");

    const auto endpoint_delete = peer.remove("/whep/live/camera");
    require(endpoint_delete.result() == boost::beast::http::status::method_not_allowed, "whep endpoint delete status");
    require(endpoint_delete[boost::beast::http::field::allow] == "GET, HEAD, POST, OPTIONS", "whep endpoint delete allow");

    const auto unavailable = peer.post("/whep/live/missing");
    require(unavailable.result() == boost::beast::http::status::conflict, "whep unavailable stream status");
    require(unavailable[boost::beast::http::field::retry_after] == "1", "whep unavailable stream retry after");
    require(unavailable["Access-Control-Allow-Origin"] == "*", "whep unavailable stream allow origin");

    const auto missing_get = peer.request(boost::beast::http::verb::get, "/whep/session/missing");
    require(missing_get.result() == boost::beast::http::status::not_found && missing_get.body().empty(), "whep missing session get");

    const auto missing_head = peer.request(boost::beast::http::verb::head, "/whep/session/missing");
    require(missing_head.result() == missing_get.result(), "whep missing session head status");
    require(missing_head[boost::beast::http::field::cache_control] == missing_get[boost::beast::http::field::cache_control],
            "whep missing session head cache control");
    require(
        missing_head[boost::beast::http::field::access_control_allow_origin] == missing_get[boost::beast::http::field::access_control_allow_origin],
        "whep missing session head allow origin");
    require(missing_head.body().empty(), "whep missing session head body");

    const auto created = peer.post("/whep/live/camera");
    require(created.result() == boost::beast::http::status::created, "whep cors create status");
    require(created["Access-Control-Allow-Origin"] == "*", "whep create allow origin");
    require(created["Access-Control-Expose-Headers"] == "Location", "whep create expose location");
    require(created[boost::beast::http::field::content_type] == "application/sdp", "whep create content type");
    const auto location = std::string(created[boost::beast::http::field::location]);
    require(location.starts_with("/whep/session/"), "whep create location");
    require(created.body().starts_with("v=0\r\n"), "whep create answer");

    require_whep_options(peer.options(location, "DELETE"), "GET, HEAD, DELETE, OPTIONS", false);

    const auto session_get = peer.request(boost::beast::http::verb::get, location);
    require(session_get.result() == boost::beast::http::status::no_content && session_get.body().empty(), "whep session get");

    const auto session_head = peer.request(boost::beast::http::verb::head, location);
    require(session_head.result() == session_get.result(), "whep session head status");
    require(session_head[boost::beast::http::field::cache_control] == session_get[boost::beast::http::field::cache_control],
            "whep session head cache control");
    require(
        session_head[boost::beast::http::field::access_control_allow_origin] == session_get[boost::beast::http::field::access_control_allow_origin],
        "whep session head allow origin");
    require(session_head.body().empty(), "whep session head body");

    const auto session_post = peer.post(location);
    require(session_post.result() == boost::beast::http::status::method_not_allowed, "whep session post status");
    require(session_post[boost::beast::http::field::allow] == "GET, HEAD, DELETE, OPTIONS", "whep session post allow");

    const auto removed = peer.remove(location);
    require(removed.result() == boost::beast::http::status::no_content, "whep cors delete status");
    require(removed["Access-Control-Allow-Origin"] == "*", "whep delete allow origin");

    const auto removed_get = peer.request(boost::beast::http::verb::get, location);
    require(removed_get.result() == boost::beast::http::status::not_found && removed_get.body().empty(), "whep removed session get");

    const auto removed_head = peer.request(boost::beast::http::verb::head, location);
    require(removed_head.result() == removed_get.result(), "whep removed session head status");
    require(removed_head[boost::beast::http::field::cache_control] == removed_get[boost::beast::http::field::cache_control],
            "whep removed session head cache control");
    require(
        removed_head[boost::beast::http::field::access_control_allow_origin] == removed_get[boost::beast::http::field::access_control_allow_origin],
        "whep removed session head allow origin");
    require(removed_head.body().empty(), "whep removed session head body");

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
                                               .stream_id = "serverstream",
                                               .ice_ufrag = "serverufrag",
                                               .ice_pwd = "serverpassword1234567890",
                                               .fingerprint = "AA:BB:CC:DD",
                                               .video = {},
                                           });
    require(answer.has_value(), "make webrtc answer");
    require(answer->video_codec == codec_id::h264, "webrtc negotiated h264 codec");
    require(answer->audio_codec == codec_id::aac, "webrtc negotiated aac source codec");
    require(answer->video_payload_type == 102, "webrtc negotiated h264 payload");
    require(answer->audio_payload_type == 111, "webrtc negotiated opus payload");
    require(answer->audio_channel_count == 2, "webrtc negotiated opus stereo");
    require(answer->audio_bitrate == 128'000, "webrtc negotiated opus default bitrate");
    require(answer->audio_max_playback_rate == 48'000, "webrtc negotiated opus default playback rate");
    require(answer->video_mid == "0" && answer->audio_mid == "1", "webrtc negotiated media mids");
    require(answer->video_mid_extension_id == 4 && answer->audio_mid_extension_id == 4, "webrtc negotiated mid extension ids");
    require(answer->sdp.find("a=ice-lite\r\n") != std::string::npos, "webrtc ice lite");
    require(answer->sdp.find("a=end-of-candidates\r\n") != std::string::npos, "webrtc complete candidates");
    require(answer->sdp.find("trickle") == std::string::npos, "webrtc no trickle");
    require(answer->sdp.find("m=video 40000 UDP/TLS/RTP/SAVPF 102\r\n") != std::string::npos, "webrtc h264 payload selection");
    require(answer->sdp.find("a=rtpmap:102 H264/90000\r\n") != std::string::npos, "webrtc h264 rtpmap");
    require(answer->sdp.find("profile-level-id=42c01f") != std::string::npos, "webrtc source h264 profile");
    require(answer->sdp.find("m=audio 40000 UDP/TLS/RTP/SAVPF 111\r\n") != std::string::npos, "webrtc bundled opus payload selection");
    require(answer->sdp.find("a=rtpmap:111 opus/48000/2\r\n") != std::string::npos, "webrtc opus rtpmap");
    require(answer->sdp.find("sprop-stereo=1") != std::string::npos, "webrtc opus stereo sender property");
    require(answer->sdp.find("a=extmap:4 urn:ietf:params:rtp-hdrext:sdes:mid\r\n") != std::string::npos, "webrtc mid extension");
    require(answer->sdp.find("a=sendonly\r\n") != std::string::npos, "webrtc sendonly");

    const std::string_view answer_sdp = answer->sdp;
    const auto video_offset = answer_sdp.find("m=video ");
    const auto audio_offset = answer_sdp.find("m=audio ");
    require(video_offset != std::string_view::npos && audio_offset != std::string_view::npos && video_offset < audio_offset,
            "webrtc bundle media order");
    const auto video_section = answer_sdp.substr(video_offset, audio_offset - video_offset);
    const auto audio_section = answer_sdp.substr(audio_offset);
    require(video_section.find("a=msid:serverstream\r\n") != std::string_view::npos &&
                audio_section.find("a=msid:serverstream\r\n") != std::string_view::npos,
            "webrtc single media stream id");
    require(video_section.find("a=rtcp-mux\r\n") != std::string_view::npos &&
                video_section.find("a=ice-ufrag:serverufrag\r\n") != std::string_view::npos &&
                video_section.find("a=fingerprint:sha-256 AA:BB:CC:DD\r\n") != std::string_view::npos &&
                video_section.find("a=setup:passive\r\n") != std::string_view::npos &&
                video_section.find("a=candidate:1 1 UDP 2130706431 127.0.0.1 40000 typ host\r\n") != std::string_view::npos &&
                video_section.find("a=end-of-candidates\r\n") != std::string_view::npos,
            "webrtc bundle tagged transport attributes");
    require(audio_section.find("a=bundle-only\r\n") == std::string_view::npos && audio_section.find("a=rtcp-mux\r\n") != std::string_view::npos &&
                audio_section.find("a=ice-ufrag:") == std::string_view::npos && audio_section.find("a=ice-pwd:") == std::string_view::npos &&
                audio_section.find("a=fingerprint:") == std::string_view::npos && audio_section.find("a=setup:") == std::string_view::npos &&
                audio_section.find("a=candidate:") == std::string_view::npos &&
                audio_section.find("a=end-of-candidates\r\n") == std::string_view::npos,
            "webrtc bundle secondary media repeats rtcp mux only");
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
                                               .stream_id = "serverstream",
                                               .ice_ufrag = "serverufrag",
                                               .ice_pwd = "serverpassword1234567890",
                                               .fingerprint = "AA:BB:CC:DD",
                                               .video = {},
                                           });
    require(answer.has_value(), "make webrtc h265 answer");
    require(answer->video_codec == codec_id::h265, "webrtc negotiated h265 codec");
    require(answer->video_payload_type == 102, "webrtc negotiated h265 payload");
    require(answer->sdp.find("a=rtpmap:102 H265/90000\r\n") != std::string::npos, "webrtc h265 rtpmap");
    require(answer->sdp.find("a=fmtp:102 profile-space=0;profile-id=1;tier-flag=0;level-id=120\r\n") != std::string::npos,
            "webrtc h265 source profile tier level");
    require(answer->sdp.find("H264/90000") == std::string::npos, "webrtc h265 answer excludes h264");
}

void test_webrtc_av1_sdp_answer()
{
    const auto make_answer = [](std::string offer_sdp)
    {
        const auto offer = parse_webrtc_offer(offer_sdp);
        require(offer.has_value(), "parse webrtc av1 offer");
        return make_webrtc_answer(*offer,
                                  {make_video_track()},
                                  webrtc_answer_config{
                                      .address = boost::asio::ip::make_address("127.0.0.1"),
                                      .port = 40000,
                                      .stream_id = "serverstream",
                                      .ice_ufrag = "serverufrag",
                                      .ice_pwd = "serverpassword1234567890",
                                      .fingerprint = "AA:BB:CC:DD",
                                      .video = output_video_config{.codec = output_video_codec::av1},
                                  });
    };

    const auto firefox = make_answer(make_av1_offer(webrtc_offer_sdp, 99, 100, {}));
    require(firefox.has_value(), "webrtc firefox av1 answer");
    require(firefox->video_codec == codec_id::av1 && firefox->video_payload_type == 99, "webrtc firefox av1 codec");
    require(firefox->sdp.find("a=rtpmap:99 AV1/90000\r\n") != std::string::npos, "webrtc firefox av1 rtpmap");
    require(firefox->sdp.find("a=fmtp:99 ") == std::string::npos, "webrtc firefox av1 keeps omitted fmtp");

    const auto chrome = make_answer(make_av1_offer(webrtc_offer_sdp, 45, 46, "level-idx=5;profile=0;tier=0"));
    require(chrome.has_value(), "webrtc chrome av1 answer");
    require(chrome->video_codec == codec_id::av1 && chrome->video_payload_type == 45, "webrtc chrome av1 codec");
    require(chrome->sdp.find("a=fmtp:45 level-idx=5;profile=0;tier=0\r\n") != std::string::npos, "webrtc chrome av1 fmtp");

    const auto profile1 = make_answer(make_av1_offer(webrtc_offer_sdp, 47, 48, "level-idx=5;profile=1;tier=0"));
    require(profile1.has_value() && profile1->video_codec == codec_id::av1, "webrtc av1 profile 1 receiver accepts profile 0 output");
}

void test_webrtc_video_codec_parameters()
{
    const auto config = webrtc_answer_config{
        .address = boost::asio::ip::make_address("127.0.0.1"),
        .port = 40000,
        .stream_id = "serverstream",
        .ice_ufrag = "serverufrag",
        .ice_pwd = "serverpassword1234567890",
        .fingerprint = "AA:BB:CC:DD",
        .video = {},
    };

    constexpr std::string_view h264_profile_level = "profile-level-id=42e01f";
    auto wrong_h264_profile_sdp = webrtc_offer_sdp;
    const auto h264_profile_offset = wrong_h264_profile_sdp.find(h264_profile_level);
    require(h264_profile_offset != std::string::npos, "webrtc h264 profile source");
    wrong_h264_profile_sdp.replace(h264_profile_offset, h264_profile_level.size(), "profile-level-id=64001f");
    const auto wrong_h264_profile = parse_webrtc_offer(wrong_h264_profile_sdp);
    require(wrong_h264_profile.has_value(), "webrtc parse incompatible h264 profile");
    const auto wrong_h264_profile_answer = make_webrtc_answer(*wrong_h264_profile, {make_video_track(), make_audio_track()}, config);
    require(!wrong_h264_profile_answer.has_value(), "webrtc reject offer with incompatible tagged h264 profile");

    auto lower_h264_level_sdp = webrtc_offer_sdp;
    const auto h264_level_offset = lower_h264_level_sdp.find(h264_profile_level);
    require(h264_level_offset != std::string::npos, "webrtc h264 level source");
    lower_h264_level_sdp.replace(h264_level_offset, h264_profile_level.size(), "profile-level-id=42e01e");
    const auto lower_h264_level = parse_webrtc_offer(lower_h264_level_sdp);
    require(lower_h264_level.has_value(), "webrtc parse lower h264 level");
    const auto lower_h264_level_answer = make_webrtc_answer(*lower_h264_level, {make_video_track(), make_audio_track()}, config);
    require(!lower_h264_level_answer.has_value(), "webrtc reject offer with tagged h264 source above offered level");

    auto wrong_h265_profile_sdp = make_h265_offer(webrtc_offer_sdp);
    const auto h265_profile_offset = wrong_h265_profile_sdp.find("profile-id=1");
    require(h265_profile_offset != std::string::npos, "webrtc h265 profile source");
    wrong_h265_profile_sdp.replace(h265_profile_offset, 12U, "profile-id=2");
    const auto wrong_h265_profile = parse_webrtc_offer(wrong_h265_profile_sdp);
    require(wrong_h265_profile.has_value(), "webrtc parse incompatible h265 profile");
    const auto wrong_h265_profile_answer = make_webrtc_answer(*wrong_h265_profile, {make_h265_track(), make_audio_track()}, config);
    require(!wrong_h265_profile_answer.has_value(), "webrtc reject offer with incompatible tagged h265 profile");

    auto lower_h265_level_sdp = make_h265_offer(webrtc_offer_sdp);
    const auto h265_level_offset = lower_h265_level_sdp.find("level-id=120");
    require(h265_level_offset != std::string::npos, "webrtc h265 level source");
    lower_h265_level_sdp.replace(h265_level_offset, 12U, "level-id=93");
    const auto lower_h265_level = parse_webrtc_offer(lower_h265_level_sdp);
    require(lower_h265_level.has_value(), "webrtc parse lower h265 level");
    const auto lower_h265_level_answer = make_webrtc_answer(*lower_h265_level, {make_h265_track(), make_audio_track()}, config);
    require(!lower_h265_level_answer.has_value(), "webrtc reject offer with tagged h265 source above offered level");

    auto wrong_h265_tx_mode_sdp = make_h265_offer(webrtc_offer_sdp);
    const auto h265_tx_mode_offset = wrong_h265_tx_mode_sdp.find("level-id=120");
    require(h265_tx_mode_offset != std::string::npos, "webrtc h265 tx mode source");
    wrong_h265_tx_mode_sdp.insert(h265_tx_mode_offset + 12U, ";tx-mode=MRST");
    const auto wrong_h265_tx_mode = parse_webrtc_offer(wrong_h265_tx_mode_sdp);
    require(wrong_h265_tx_mode.has_value(), "webrtc parse incompatible h265 tx mode");
    const auto wrong_h265_tx_mode_answer = make_webrtc_answer(*wrong_h265_tx_mode, {make_h265_track(), make_audio_track()}, config);
    require(!wrong_h265_tx_mode_answer.has_value(), "webrtc reject offer with unsupported tagged h265 tx mode");

    auto h265_compatibility_sdp = make_h265_offer(webrtc_offer_sdp);
    const auto h265_compatibility_offset = h265_compatibility_sdp.find("level-id=120");
    require(h265_compatibility_offset != std::string::npos, "webrtc h265 compatibility source");
    h265_compatibility_sdp.insert(h265_compatibility_offset + 12U, ";profile-compatibility-indicator=00000000");
    const auto h265_compatibility = parse_webrtc_offer(h265_compatibility_sdp);
    require(h265_compatibility.has_value(), "webrtc parse unsupported h265 compatibility");
    const auto h265_compatibility_answer = make_webrtc_answer(*h265_compatibility, {make_h265_track(), make_audio_track()}, config);
    require(!h265_compatibility_answer.has_value(), "webrtc reject offer with unsupported tagged h265 compatibility");

    auto h265_constraints_sdp = make_h265_offer(webrtc_offer_sdp);
    const auto h265_constraints_offset = h265_constraints_sdp.find("level-id=120");
    require(h265_constraints_offset != std::string::npos, "webrtc h265 constraints source");
    h265_constraints_sdp.insert(h265_constraints_offset + 12U, ";interop-constraints=000000000000");
    const auto h265_constraints = parse_webrtc_offer(h265_constraints_sdp);
    require(h265_constraints.has_value(), "webrtc parse unsupported h265 constraints");
    const auto h265_constraints_answer = make_webrtc_answer(*h265_constraints, {make_h265_track(), make_audio_track()}, config);
    require(!h265_constraints_answer.has_value(), "webrtc reject offer with unsupported tagged h265 constraints");
}

void test_webrtc_payload_type_membership()
{
    const auto config = webrtc_answer_config{
        .address = boost::asio::ip::make_address("127.0.0.1"),
        .port = 40000,
        .stream_id = "serverstream",
        .ice_ufrag = "serverufrag",
        .ice_pwd = "serverpassword1234567890",
        .fingerprint = "AA:BB:CC:DD",
        .video = {},
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
    require(!h264_answer.has_value(), "webrtc reject offer with unoffered tagged h264 payload");

    auto h265_sdp = h264_sdp;
    h265_sdp.replace(h265_sdp.find("H264/90000"), 10U, "H265/90000");
    const auto h265_answer = answer_for(h265_sdp, {make_h265_track(), make_audio_track()});
    require(!h265_answer.has_value(), "webrtc reject offer with unoffered tagged h265 payload");

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
        .stream_id = "serverstream",
        .ice_ufrag = "serverufrag",
        .ice_pwd = "serverpassword1234567890",
        .fingerprint = "AA:BB:CC:DD",
        .video = {},
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
    const auto audio_offer = parse_webrtc_offer(make_audio_tag_offer(invalid_sdp));
    require(audio_offer.has_value(), "webrtc parse out of range audio payload offer");
    require(!make_webrtc_answer(*audio_offer, {make_audio_track()}, config).has_value(), "webrtc reject out of range opus payload");

    replace("H264/90000", "H265/90000");
    const auto h265_offer = parse_webrtc_offer(invalid_sdp);
    require(h265_offer.has_value(), "webrtc parse out of range h265 payload offer");
    require(!make_webrtc_answer(*h265_offer, {make_h265_track()}, config).has_value(), "webrtc reject out of range h265 payload");

    auto reserved_h264_sdp = webrtc_offer_sdp;
    const auto replace_reserved_h264 = [&reserved_h264_sdp](std::string_view source, std::string_view target)
    {
        const auto offset = reserved_h264_sdp.find(source);
        require(offset != std::string::npos, "webrtc rtcp mux h264 payload source");
        reserved_h264_sdp.replace(offset, source.size(), target);
    };
    replace_reserved_h264("m=video 9 UDP/TLS/RTP/SAVPF 102 127", "m=video 9 UDP/TLS/RTP/SAVPF 72 127");
    replace_reserved_h264("a=rtpmap:102 H264/90000", "a=rtpmap:72 H264/90000");
    replace_reserved_h264("a=fmtp:102 ", "a=fmtp:72 ");
    const auto reserved_h264_offer = parse_webrtc_offer(reserved_h264_sdp);
    require(reserved_h264_offer.has_value(), "webrtc parse rtcp mux reserved h264 payload");
    require(!make_webrtc_answer(*reserved_h264_offer, {make_video_track()}, config).has_value(), "webrtc reject rtcp mux reserved h264 payload");

    auto reserved_h265_sdp = reserved_h264_sdp;
    reserved_h265_sdp.replace(reserved_h265_sdp.find("H264/90000"), 10U, "H265/90000");
    const auto reserved_h265_offer = parse_webrtc_offer(reserved_h265_sdp);
    require(reserved_h265_offer.has_value(), "webrtc parse rtcp mux reserved h265 payload");
    require(!make_webrtc_answer(*reserved_h265_offer, {make_h265_track()}, config).has_value(), "webrtc reject rtcp mux reserved h265 payload");

    auto reserved_opus_sdp = make_audio_tag_offer(webrtc_offer_sdp);
    const auto replace_reserved_opus = [&reserved_opus_sdp](std::string_view source, std::string_view target)
    {
        const auto offset = reserved_opus_sdp.find(source);
        require(offset != std::string::npos, "webrtc rtcp mux opus payload source");
        reserved_opus_sdp.replace(offset, source.size(), target);
    };
    replace_reserved_opus("m=audio 9 UDP/TLS/RTP/SAVPF 111 0 8", "m=audio 9 UDP/TLS/RTP/SAVPF 95 0 8");
    replace_reserved_opus("a=rtpmap:111 opus/48000/2", "a=rtpmap:95 opus/48000/2");
    replace_reserved_opus("a=fmtp:111 ", "a=fmtp:95 ");
    const auto reserved_opus_offer = parse_webrtc_offer(reserved_opus_sdp);
    require(reserved_opus_offer.has_value(), "webrtc parse rtcp mux reserved opus payload");
    require(!make_webrtc_answer(*reserved_opus_offer, {make_audio_track()}, config).has_value(), "webrtc reject rtcp mux reserved opus payload");
}

void test_webrtc_disabled_media()
{
    const auto config = webrtc_answer_config{
        .address = boost::asio::ip::make_address("127.0.0.1"),
        .port = 40000,
        .stream_id = "serverstream",
        .ice_ufrag = "serverufrag",
        .ice_pwd = "serverpassword1234567890",
        .fingerprint = "AA:BB:CC:DD",
        .video = {},
    };
    auto disabled_video_sdp = webrtc_offer_sdp;
    disabled_video_sdp.replace(disabled_video_sdp.find("m=video 9 "), 10U, "m=video 0 ");
    const auto disabled_video = parse_webrtc_offer(disabled_video_sdp);
    require(disabled_video.has_value(), "webrtc parse disabled video offer");
    require(!make_webrtc_answer(*disabled_video, {make_video_track()}, config).has_value(), "webrtc reject disabled tagged h264");

    disabled_video_sdp.replace(disabled_video_sdp.find("H264/90000"), 10U, "H265/90000");
    const auto disabled_h265 = parse_webrtc_offer(disabled_video_sdp);
    require(disabled_h265.has_value(), "webrtc parse disabled h265 offer");
    require(!make_webrtc_answer(*disabled_h265, {make_h265_track()}, config).has_value(), "webrtc reject disabled tagged h265");

    auto disabled_audio_sdp = make_audio_tag_offer(webrtc_offer_sdp);
    disabled_audio_sdp.replace(disabled_audio_sdp.find("m=audio 9 "), 10U, "m=audio 0 ");
    const auto disabled_audio = parse_webrtc_offer(disabled_audio_sdp);
    require(disabled_audio.has_value(), "webrtc parse disabled audio offer");
    require(!make_webrtc_answer(*disabled_audio, {make_audio_track()}, config).has_value(), "webrtc reject disabled tagged opus");

    auto bundle_only_audio_sdp = webrtc_offer_sdp;
    bundle_only_audio_sdp.replace(bundle_only_audio_sdp.find("m=audio 9 "), 10U, "m=audio 0 ");
    const auto disabled_secondary_audio = parse_webrtc_offer(bundle_only_audio_sdp);
    require(disabled_secondary_audio.has_value(), "webrtc parse disabled secondary audio offer");
    const auto disabled_secondary_answer = make_webrtc_answer(*disabled_secondary_audio, {make_video_track(), make_audio_track()}, config);
    require(disabled_secondary_answer.has_value() && disabled_secondary_answer->video_payload_type == 102 &&
                !disabled_secondary_answer->audio_payload_type.has_value() &&
                disabled_secondary_answer->sdp.find("a=group:BUNDLE 0\r\n") != std::string::npos,
            "webrtc keep disabled secondary audio rejected");

    const std::string mid = "a=mid:1\r\n";
    const auto mid_offset = bundle_only_audio_sdp.find(mid);
    require(mid_offset != std::string::npos, "webrtc bundle only audio source");
    bundle_only_audio_sdp.replace(mid_offset, mid.size(), mid + "a=bundle-only\r\n");
    const auto bundle_only_audio = parse_webrtc_offer(bundle_only_audio_sdp);
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
        .stream_id = "serverstream",
        .ice_ufrag = "serverufrag",
        .ice_pwd = "serverpassword1234567890",
        .fingerprint = "AA:BB:CC:DD",
        .video = {},
    };
    const auto offer = parse_webrtc_offer(duplicate_sdp);
    require(offer.has_value(), "webrtc parse duplicate media offer");
    const auto answer = make_webrtc_answer(*offer, {make_video_track(), make_audio_track()}, config);
    require(answer.has_value(), "webrtc answer duplicate media offer");
    require(answer->video_payload_type == 102, "webrtc accept one video media");
    require(answer->audio_payload_type == 111, "webrtc accept one audio media");
    require(answer->sdp.find("m=video 0 UDP/TLS/RTP/SAVPF 103\r\n") != std::string::npos, "webrtc reject duplicate video media");
    require(answer->sdp.find("m=audio 0 UDP/TLS/RTP/SAVPF 112\r\n") != std::string::npos, "webrtc reject duplicate audio media");

    auto later_video_tag_sdp = duplicate_sdp;
    const auto later_tag_bundle = later_video_tag_sdp.find("a=group:BUNDLE 0 1 2 3\r\n");
    const auto later_tag_mid = later_video_tag_sdp.find("a=mid:2\r\n");
    require(later_tag_bundle != std::string::npos && later_tag_mid != std::string::npos, "webrtc later video tag source");
    later_video_tag_sdp.replace(later_tag_bundle, std::string_view("a=group:BUNDLE 0 1 2 3\r\n").size(), "a=group:BUNDLE 2 0 1 3\r\n");
    later_video_tag_sdp.insert(
        later_tag_mid + std::string_view("a=mid:2\r\n").size(),
        "a=ice-ufrag:remotevideo2\r\n"
        "a=ice-pwd:remotevideo2password123456\r\n"
        "a=fingerprint:sha-256 00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF\r\n"
        "a=setup:actpass\r\n"
        "a=extmap:4 urn:ietf:params:rtp-hdrext:sdes:mid\r\n");
    const auto later_video_tag = parse_webrtc_offer(later_video_tag_sdp);
    require(later_video_tag.has_value(), "webrtc parse later video bundle tag");
    const auto later_video_tag_answer = make_webrtc_answer(*later_video_tag, {make_video_track(), make_audio_track()}, config);
    require(later_video_tag_answer.has_value() && later_video_tag_answer->transport_mid == "2" && later_video_tag_answer->video_mid == "2" &&
                later_video_tag_answer->video_payload_type == 103 && later_video_tag_answer->audio_payload_type == 111 &&
                later_video_tag_answer->sdp.find("a=group:BUNDLE 2 1\r\n") != std::string::npos,
            "webrtc bundle tag reserves matching media kind");
    require(later_video_tag_answer->sdp.find("m=video 0 UDP/TLS/RTP/SAVPF 102 127\r\n") != std::string::npos,
            "webrtc reject earlier same kind media before bundle tag");

    auto h265_sdp = make_h265_offer(duplicate_sdp);
    constexpr std::string_view h264_rtpmap_103 = "a=rtpmap:103 H264/90000\r\n";
    constexpr std::string_view h264_fmtp_103 = "a=fmtp:103 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f\r\n";
    const auto h264_rtpmap_103_offset = h265_sdp.find(h264_rtpmap_103);
    const auto h264_fmtp_103_offset = h265_sdp.find(h264_fmtp_103);
    require(h264_rtpmap_103_offset != std::string::npos && h264_fmtp_103_offset != std::string::npos, "webrtc duplicate h265 source");
    h265_sdp.replace(h264_rtpmap_103_offset, h264_rtpmap_103.size(), "a=rtpmap:103 H265/90000\r\n");
    h265_sdp.replace(h264_fmtp_103_offset, h264_fmtp_103.size(), "a=fmtp:103 profile-space=0;profile-id=1;tier-flag=0;level-id=120\r\n");
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
                                               .stream_id = "serverstream",
                                               .ice_ufrag = "serverufrag",
                                               .ice_pwd = "serverpassword1234567890",
                                               .fingerprint = "AA:BB:CC:DD",
                                               .video = {},
                                           });
    require(answer.has_value(), "make webrtc mono answer");
    require(answer->audio_channel_count == 1, "webrtc opus mono default");
    require(answer->sdp.find("sprop-stereo=0") != std::string::npos, "webrtc opus mono sender property");
}

void test_webrtc_opus_receive_limits()
{
    const auto config = webrtc_answer_config{
        .address = boost::asio::ip::make_address("127.0.0.1"),
        .port = 40000,
        .stream_id = "serverstream",
        .ice_ufrag = "serverufrag",
        .ice_pwd = "serverpassword1234567890",
        .fingerprint = "AA:BB:CC:DD",
        .video = {},
    };

    auto limited_sdp = make_audio_tag_offer(webrtc_offer_sdp);
    const std::string opus_fmtp = "a=fmtp:111 minptime=10;useinbandfec=1;stereo=1\r\n";
    const auto fmtp_offset = limited_sdp.find(opus_fmtp);
    require(fmtp_offset != std::string::npos, "webrtc opus limit source");
    limited_sdp.replace(
        fmtp_offset, opus_fmtp.size(), "a=fmtp:111 minptime=10;useinbandfec=1;stereo=1;maxaveragebitrate=32000;maxplaybackrate=16000\r\n");
    const auto limited_offer = parse_webrtc_offer(limited_sdp);
    require(limited_offer.has_value(), "webrtc parse opus receiver limits");
    const auto limited_answer = make_webrtc_answer(*limited_offer, {make_audio_track()}, config);
    require(limited_answer.has_value(), "webrtc answer opus receiver limits");
    require(limited_answer->audio_channel_count == 2, "webrtc opus receiver stereo");
    require(limited_answer->audio_bitrate == 32'000, "webrtc opus receiver max bitrate");
    require(limited_answer->audio_max_playback_rate == 16'000, "webrtc opus receiver max playback rate");

    auto low_bitrate_sdp = make_audio_tag_offer(webrtc_offer_sdp);
    const auto low_bitrate_fmtp_offset = low_bitrate_sdp.find(opus_fmtp);
    require(low_bitrate_fmtp_offset != std::string::npos, "webrtc opus low bitrate source");
    low_bitrate_sdp.replace(low_bitrate_fmtp_offset, opus_fmtp.size(), "a=fmtp:111 minptime=10;useinbandfec=1;stereo=1;maxaveragebitrate=5999\r\n");
    const auto low_bitrate_offer = parse_webrtc_offer(low_bitrate_sdp);
    require(low_bitrate_offer.has_value(), "webrtc parse opus low receiver bitrate");
    const auto low_bitrate_answer = make_webrtc_answer(*low_bitrate_offer, {make_audio_track()}, config);
    require(low_bitrate_answer.has_value() && low_bitrate_answer->audio_bitrate == 6'000, "webrtc clamp opus low receiver bitrate");

    auto missing_channels_sdp = make_audio_tag_offer(webrtc_offer_sdp);
    const std::string opus_rtpmap = "a=rtpmap:111 opus/48000/2\r\n";
    const auto rtpmap_offset = missing_channels_sdp.find(opus_rtpmap);
    require(rtpmap_offset != std::string::npos, "webrtc opus channels source");
    missing_channels_sdp.replace(rtpmap_offset, opus_rtpmap.size(), "a=rtpmap:111 opus/48000\r\n");
    const auto missing_channels = parse_webrtc_offer(missing_channels_sdp);
    require(missing_channels.has_value(), "webrtc parse opus missing channels");
    require(!make_webrtc_answer(*missing_channels, {make_audio_track()}, config).has_value(), "webrtc reject opus missing channels");
}

void test_webrtc_opus_source_negotiation()
{
    auto compatible_sdp = webrtc_offer_sdp;
    const auto compatible_fmtp = compatible_sdp.find("a=fmtp:111 minptime=10;useinbandfec=1;stereo=1\r\n");
    require(compatible_fmtp != std::string::npos, "webrtc opus source compatible fmtp");
    compatible_sdp.replace(compatible_fmtp,
                           std::string_view("a=fmtp:111 minptime=10;useinbandfec=1;stereo=1\r\n").size(),
                           "a=fmtp:111 minptime=10;useinbandfec=1;stereo=1;maxaveragebitrate=510000\r\n");
    const auto offer = parse_webrtc_offer(compatible_sdp);
    require(offer.has_value(), "parse webrtc opus source offer");
    const auto config = webrtc_answer_config{
        .address = boost::asio::ip::make_address("127.0.0.1"),
        .port = 40000,
        .stream_id = "serverstream",
        .ice_ufrag = "serverufrag",
        .ice_pwd = "serverpassword1234567890",
        .fingerprint = "AA:BB:CC:DD",
        .video = {},
    };

    const auto stereo = make_webrtc_answer(*offer, {make_video_track(), make_opus_track(2)}, config);
    require(stereo.has_value() && stereo->audio_codec == codec_id::opus && stereo->audio_payload_type == 111 && stereo->audio_channel_count == 2,
            "webrtc negotiate stereo opus source");
    require(stereo->sdp.find("a=rtpmap:111 opus/48000/2\r\n") != std::string::npos && stereo->sdp.find("sprop-stereo=1") != std::string::npos,
            "webrtc stereo opus source answer");

    const auto mono = make_webrtc_answer(*offer, {make_video_track(), make_opus_track(1)}, config);
    require(mono.has_value() && mono->audio_codec == codec_id::opus && mono->audio_channel_count == 1, "webrtc negotiate mono opus source");
    require(mono->sdp.find("a=rtpmap:111 opus/48000/2\r\n") != std::string::npos && mono->sdp.find("sprop-stereo=0") != std::string::npos,
            "webrtc mono opus keeps rfc mapping");

    auto no_bitrate_sdp = compatible_sdp;
    const auto bitrate_parameter = no_bitrate_sdp.find(";maxaveragebitrate=510000");
    require(bitrate_parameter != std::string::npos, "webrtc opus passthrough bitrate parameter");
    no_bitrate_sdp.erase(bitrate_parameter, std::string_view(";maxaveragebitrate=510000").size());
    const auto no_bitrate_offer = parse_webrtc_offer(no_bitrate_sdp);
    require(no_bitrate_offer.has_value(), "parse webrtc opus passthrough default bitrate");
    const auto no_bitrate = make_webrtc_answer(*no_bitrate_offer, {make_video_track(), make_opus_track(2)}, config);
    require(no_bitrate.has_value() && no_bitrate->video_codec == codec_id::h264 && !no_bitrate->audio_codec,
            "webrtc reject opus passthrough default bitrate limit");

    auto limited_bitrate_sdp = compatible_sdp;
    const auto limited_bitrate = limited_bitrate_sdp.find("maxaveragebitrate=510000");
    require(limited_bitrate != std::string::npos, "webrtc opus passthrough limited bitrate parameter");
    limited_bitrate_sdp.replace(limited_bitrate, std::string_view("maxaveragebitrate=510000").size(), "maxaveragebitrate=128000");
    const auto limited_bitrate_offer = parse_webrtc_offer(limited_bitrate_sdp);
    require(limited_bitrate_offer.has_value(), "parse webrtc opus passthrough limited bitrate");
    const auto incompatible_bitrate = make_webrtc_answer(*limited_bitrate_offer, {make_video_track(), make_opus_track(2)}, config);
    require(incompatible_bitrate.has_value() && !incompatible_bitrate->audio_codec, "webrtc reject opus passthrough bitrate limit");

    for (const int maxptime : {20, 40, 60})
    {
        auto maxptime_sdp = compatible_sdp;
        const auto audio_mid = maxptime_sdp.find("a=mid:1\r\n");
        require(audio_mid != std::string::npos, "webrtc opus maxptime audio media");
        maxptime_sdp.insert(audio_mid, "a=maxptime:" + std::to_string(maxptime) + "\r\n");
        const auto maxptime_offer = parse_webrtc_offer(maxptime_sdp);
        require(maxptime_offer.has_value() && maxptime_offer->media.back().max_packet_time_ms == maxptime, "parse webrtc opus maxptime");
        const auto incompatible_maxptime = make_webrtc_answer(*maxptime_offer, {make_video_track(), make_opus_track(2)}, config);
        require(incompatible_maxptime.has_value() && !incompatible_maxptime->audio_codec, "webrtc reject opus passthrough maxptime limit");
    }

    auto maxptime_sdp = compatible_sdp;
    const auto audio_mid = maxptime_sdp.find("a=mid:1\r\n");
    require(audio_mid != std::string::npos, "webrtc opus maxptime 120 audio media");
    maxptime_sdp.insert(audio_mid, "a=maxptime:120\r\n");
    const auto maxptime_offer = parse_webrtc_offer(maxptime_sdp);
    require(maxptime_offer.has_value() && maxptime_offer->media.back().max_packet_time_ms == 120, "parse webrtc opus maxptime 120");
    const auto compatible_maxptime = make_webrtc_answer(*maxptime_offer, {make_video_track(), make_opus_track(2)}, config);
    require(compatible_maxptime.has_value() && compatible_maxptime->audio_codec == codec_id::opus, "webrtc accept opus passthrough maxptime 120");

    const auto audio_rejected = [&offer, &config](media_track track)
    {
        const auto answer = make_webrtc_answer(*offer, {make_video_track(), std::move(track)}, config);
        return answer && !answer->audio_codec && !answer->audio_payload_type &&
               answer->sdp.find("m=audio 0 UDP/TLS/RTP/SAVPF 111 0 8\r\n") != std::string::npos;
    };

    auto invalid_rate = make_opus_track();
    invalid_rate.clock_rate = 44'100;
    require(audio_rejected(std::move(invalid_rate)), "webrtc reject opus source rate");
    auto invalid_config = make_opus_track();
    invalid_config.codec_config = {0x01};
    require(audio_rejected(std::move(invalid_config)), "webrtc reject opus source config");
    require(audio_rejected(make_opus_track(0)) && audio_rejected(make_opus_track(3)), "webrtc reject opus source channels");

    auto mono_offer_sdp = compatible_sdp;
    const auto stereo_parameter = mono_offer_sdp.find(";stereo=1");
    require(stereo_parameter != std::string::npos, "webrtc opus source stereo parameter");
    mono_offer_sdp.erase(stereo_parameter, std::string_view(";stereo=1").size());
    const auto mono_offer = parse_webrtc_offer(mono_offer_sdp);
    require(mono_offer.has_value(), "parse webrtc mono receiver offer");
    const auto incompatible_stereo = make_webrtc_answer(*mono_offer, {make_video_track(), make_opus_track(2)}, config);
    require(incompatible_stereo.has_value() && !incompatible_stereo->audio_codec, "webrtc reject stereo opus for mono receiver");

    auto limited_sdp = compatible_sdp;
    const auto fmtp = limited_sdp.find("a=fmtp:111 minptime=10;useinbandfec=1;stereo=1;maxaveragebitrate=510000\r\n");
    require(fmtp != std::string::npos, "webrtc opus passthrough limit source");
    limited_sdp.replace(fmtp,
                        std::string_view("a=fmtp:111 minptime=10;useinbandfec=1;stereo=1;maxaveragebitrate=510000\r\n").size(),
                        "a=fmtp:111 minptime=10;useinbandfec=1;stereo=1;maxaveragebitrate=510000;maxplaybackrate=16000\r\n");
    const auto limited_offer = parse_webrtc_offer(limited_sdp);
    require(limited_offer.has_value(), "parse webrtc opus passthrough limit");
    const auto incompatible_rate = make_webrtc_answer(*limited_offer, {make_video_track(), make_opus_track(2)}, config);
    require(incompatible_rate.has_value() && !incompatible_rate->audio_codec, "webrtc reject opus passthrough playback limit");
}

void test_webrtc_g711_source_negotiation()
{
    const auto config = webrtc_answer_config{
        .address = boost::asio::ip::make_address("127.0.0.1"),
        .port = 40000,
        .stream_id = "serverstream",
        .ice_ufrag = "serverufrag",
        .ice_pwd = "serverpassword1234567890",
        .fingerprint = "AA:BB:CC:DD",
        .video = {},
    };
    const auto check = [&config](codec_id codec, std::string sdp, int payload_type)
    {
        const auto offer = parse_webrtc_offer(sdp);
        require(offer.has_value(), "parse webrtc g711 offer");
        const auto answer = make_webrtc_answer(*offer, {make_video_track(), make_g711_track(codec)}, config);
        require(answer.has_value() && answer->video_codec == codec_id::h264 && answer->audio_codec == codec &&
                    answer->audio_payload_type == payload_type && answer->audio_channel_count == 1,
                "webrtc negotiate g711 source");
        require(answer->sdp.find("m=audio 40000 UDP/TLS/RTP/SAVPF " + std::to_string(payload_type) + "\r\n") != std::string::npos,
                "webrtc g711 static payload answer");
    };

    check(codec_id::g711u, webrtc_offer_sdp, RTP_PAYLOAD_PCMU);
    check(codec_id::g711a, webrtc_offer_sdp, RTP_PAYLOAD_PCMA);

    auto implicit_sdp = webrtc_offer_sdp;
    const auto pcmu = implicit_sdp.find("a=rtpmap:0 PCMU/8000\r\n");
    const auto pcma = implicit_sdp.find("a=rtpmap:8 PCMA/8000\r\n");
    require(pcmu != std::string::npos && pcma != std::string::npos, "webrtc g711 explicit rtpmap source");
    implicit_sdp.erase(pcma, std::string_view("a=rtpmap:8 PCMA/8000\r\n").size());
    implicit_sdp.erase(pcmu, std::string_view("a=rtpmap:0 PCMU/8000\r\n").size());
    check(codec_id::g711u, implicit_sdp, RTP_PAYLOAD_PCMU);
    check(codec_id::g711a, implicit_sdp, RTP_PAYLOAD_PCMA);

    auto mismatch_sdp = webrtc_offer_sdp;
    const auto mismatch = mismatch_sdp.find("a=rtpmap:8 PCMA/8000\r\n");
    require(mismatch != std::string::npos, "webrtc g711 mismatch source");
    mismatch_sdp.replace(mismatch, std::string_view("a=rtpmap:8 PCMA/8000\r\n").size(), "a=rtpmap:8 PCMU/8000\r\n");
    const auto mismatch_offer = parse_webrtc_offer(mismatch_sdp);
    require(mismatch_offer.has_value(), "parse webrtc mismatched g711 offer");
    const auto mismatch_answer = make_webrtc_answer(*mismatch_offer, {make_video_track(), make_g711_track(codec_id::g711a)}, config);
    require(mismatch_answer.has_value() && mismatch_answer->video_codec == codec_id::h264 && !mismatch_answer->audio_codec,
            "webrtc reject mismatched g711 audio only");

    const auto audio_tag_offer = parse_webrtc_offer(make_audio_tag_offer(webrtc_offer_sdp));
    require(audio_tag_offer.has_value(), "parse webrtc g711 audio tag offer");
    for (const auto codec : {codec_id::g711a, codec_id::g711u})
    {
        auto invalid = make_g711_track(codec);
        invalid.clock_rate = 16'000;
        require(!make_webrtc_answer(*audio_tag_offer, {std::move(invalid)}, config).has_value(), "webrtc reject g711 rate");
        invalid = make_g711_track(codec);
        invalid.channel_count = 2;
        require(!make_webrtc_answer(*audio_tag_offer, {std::move(invalid)}, config).has_value(), "webrtc reject g711 channels");
        invalid = make_g711_track(codec);
        invalid.codec_config = {1};
        require(!make_webrtc_answer(*audio_tag_offer, {std::move(invalid)}, config).has_value(), "webrtc reject g711 config");
    }
}

void test_webrtc_transport_contract()
{
    const auto config = webrtc_answer_config{
        .address = boost::asio::ip::make_address("127.0.0.1"),
        .port = 40000,
        .stream_id = "serverstream",
        .ice_ufrag = "serverufrag",
        .ice_pwd = "serverpassword1234567890",
        .fingerprint = "AA:BB:CC:DD",
        .video = {},
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

    auto multiple_group_sdp = webrtc_offer_sdp;
    const auto multiple_group_offset = multiple_group_sdp.find(bundle);
    require(multiple_group_offset != std::string::npos, "webrtc multiple group offer");
    multiple_group_sdp.insert(multiple_group_offset, "a=group:LS 0 1\r\n");
    const auto multiple_group = parse_webrtc_offer(multiple_group_sdp);
    require(multiple_group.has_value() && multiple_group->bundle_mids == std::vector<std::string>{"0", "1"},
            "webrtc find bundle among multiple group semantics");
    require(make_webrtc_answer(*multiple_group, tracks, config).has_value(), "webrtc answer offer with non bundle group before bundle");

    auto multiple_bundle_sdp = webrtc_offer_sdp;
    const auto multiple_bundle_offset = multiple_bundle_sdp.find(bundle);
    require(multiple_bundle_offset != std::string::npos, "webrtc multiple bundle offer");
    multiple_bundle_sdp.insert(multiple_bundle_offset + bundle.size(), "a=group:BUNDLE 0 1\r\n");
    require(!parse_webrtc_offer(multiple_bundle_sdp).has_value(), "webrtc reject multiple bundle groups");

    auto no_rtcp_mux_sdp = webrtc_offer_sdp;
    const std::string rtcp_mux = "a=rtcp-mux\r\n";
    const auto rtcp_mux_offset = no_rtcp_mux_sdp.find(rtcp_mux);
    require(rtcp_mux_offset != std::string::npos, "webrtc rtcp mux offer");
    no_rtcp_mux_sdp.erase(rtcp_mux_offset, rtcp_mux.size());
    const auto no_rtcp_mux = parse_webrtc_offer(no_rtcp_mux_sdp);
    require(no_rtcp_mux.has_value(), "webrtc parse no rtcp mux offer");
    const auto no_rtcp_mux_answer = make_webrtc_answer(*no_rtcp_mux, tracks, config);
    require(!no_rtcp_mux_answer.has_value(), "webrtc reject offer without rtcp mux on bundle tag");

    auto wrong_protocol_sdp = webrtc_offer_sdp;
    const std::string protocol = "UDP/TLS/RTP/SAVPF";
    const auto protocol_offset = wrong_protocol_sdp.find(protocol);
    require(protocol_offset != std::string::npos, "webrtc protocol offer");
    wrong_protocol_sdp.replace(protocol_offset, protocol.size(), "RTP/AVP");
    const auto wrong_protocol = parse_webrtc_offer(wrong_protocol_sdp);
    require(wrong_protocol.has_value(), "webrtc parse wrong protocol offer");
    const auto wrong_protocol_answer = make_webrtc_answer(*wrong_protocol, tracks, config);
    require(!wrong_protocol_answer.has_value(), "webrtc reject offer without dtls srtp on bundle tag");

    auto passive_offer_sdp = webrtc_offer_sdp;
    const std::string actpass = "a=setup:actpass\r\n";
    const auto setup_offset = passive_offer_sdp.find(actpass);
    require(setup_offset != std::string::npos, "webrtc setup offer");
    passive_offer_sdp.replace(setup_offset, actpass.size(), "a=setup:passive\r\n");
    const auto passive_offer = parse_webrtc_offer(passive_offer_sdp);
    require(passive_offer.has_value(), "webrtc parse passive setup offer");
    const auto passive_answer = make_webrtc_answer(*passive_offer, tracks, config);
    require(!passive_answer.has_value(), "webrtc reject offer with passive initial setup on bundle tag");

    auto active_offer_sdp = webrtc_offer_sdp;
    const auto active_setup_offset = active_offer_sdp.find(actpass);
    require(active_setup_offset != std::string::npos, "webrtc active setup offer");
    active_offer_sdp.replace(active_setup_offset, actpass.size(), "a=setup:active\r\n");
    const auto active_offer = parse_webrtc_offer(active_offer_sdp);
    require(active_offer.has_value(), "webrtc parse active setup offer");
    const auto active_answer = make_webrtc_answer(*active_offer, tracks, config);
    require(!active_answer.has_value(), "webrtc reject offer with active initial setup on bundle tag");

    const std::string audio_transport =
        "m=audio 9 UDP/TLS/RTP/SAVPF 111 0 8\r\n"
        "c=IN IP4 0.0.0.0\r\n"
        "a=ice-ufrag:remoteaudio\r\n"
        "a=ice-pwd:remoteaudiopassword123456\r\n"
        "a=fingerprint:sha-256 00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF\r\n"
        "a=setup:actpass\r\n"
        "a=mid:1\r\n"
        "a=extmap:4 urn:ietf:params:rtp-hdrext:sdes:mid\r\n"
        "a=recvonly\r\n"
        "a=rtcp-mux\r\n";
    const std::string bundle_only_audio =
        "m=audio 0 UDP/TLS/RTP/SAVPF 111 0 8\r\n"
        "c=IN IP4 0.0.0.0\r\n"
        "a=mid:1\r\n"
        "a=bundle-only\r\n"
        "a=extmap:4 urn:ietf:params:rtp-hdrext:sdes:mid\r\n"
        "a=recvonly\r\n";
    auto bundle_only_offer_sdp = webrtc_offer_sdp;
    const auto audio_transport_offset = bundle_only_offer_sdp.find(audio_transport);
    require(audio_transport_offset != std::string::npos, "webrtc bundle only audio transport source");
    bundle_only_offer_sdp.replace(audio_transport_offset, audio_transport.size(), bundle_only_audio);
    const auto bundle_only_offer = parse_webrtc_offer(bundle_only_offer_sdp);
    require(bundle_only_offer.has_value(), "webrtc parse bundle only offer without transport attributes");
    const auto bundle_only_answer = make_webrtc_answer(*bundle_only_offer, tracks, config);
    require(bundle_only_answer.has_value() && bundle_only_answer->transport_mid == "0" && bundle_only_answer->video_payload_type == 102 &&
                bundle_only_answer->audio_payload_type == 111,
            "webrtc accept initial bundle only media without transport attributes");
    const auto bundle_only_audio_offset = bundle_only_answer->sdp.find("m=audio 40000 UDP/TLS/RTP/SAVPF 111\r\n");
    require(bundle_only_audio_offset != std::string::npos &&
                bundle_only_answer->sdp.find("a=bundle-only\r\n", bundle_only_audio_offset) == std::string::npos,
            "webrtc answer assigns shared port to accepted bundle only media");

    auto invalid_bundle_only_sdp = webrtc_offer_sdp;
    const auto invalid_bundle_only_mid = invalid_bundle_only_sdp.find("a=mid:1\r\n");
    require(invalid_bundle_only_mid != std::string::npos, "webrtc invalid bundle only source");
    invalid_bundle_only_sdp.insert(invalid_bundle_only_mid + std::string_view("a=mid:1\r\n").size(), "a=bundle-only\r\n");
    const auto invalid_bundle_only = parse_webrtc_offer(invalid_bundle_only_sdp);
    require(invalid_bundle_only.has_value(), "webrtc parse nonzero bundle only offer");
    const auto invalid_bundle_only_answer = make_webrtc_answer(*invalid_bundle_only, tracks, config);
    require(invalid_bundle_only_answer.has_value() && invalid_bundle_only_answer->video_payload_type == 102 &&
                !invalid_bundle_only_answer->audio_payload_type.has_value(),
            "webrtc reject bundle only media with nonzero port");

    auto audio_tag_sdp = webrtc_offer_sdp;
    const auto audio_tag_bundle_offset = audio_tag_sdp.find(bundle);
    require(audio_tag_bundle_offset != std::string::npos, "webrtc audio tagged bundle source");
    audio_tag_sdp.replace(audio_tag_bundle_offset, bundle.size(), "a=group:BUNDLE 1 0\r\n");
    const auto audio_tag_offer = parse_webrtc_offer(audio_tag_sdp);
    require(audio_tag_offer.has_value(), "webrtc parse audio tagged bundle offer");
    const auto audio_tag_answer = make_webrtc_answer(*audio_tag_offer, tracks, config);
    require(audio_tag_answer.has_value() && audio_tag_answer->transport_mid == "1", "webrtc select audio tagged transport");
    const std::string_view audio_tag_answer_sdp = audio_tag_answer->sdp;
    const auto audio_tag_video_offset = audio_tag_answer_sdp.find("m=video ");
    const auto audio_tag_audio_offset = audio_tag_answer_sdp.find("m=audio ");
    require(audio_tag_video_offset != std::string_view::npos && audio_tag_audio_offset != std::string_view::npos &&
                audio_tag_video_offset < audio_tag_audio_offset,
            "webrtc audio tagged media order");
    const auto untagged_video_section = audio_tag_answer_sdp.substr(audio_tag_video_offset, audio_tag_audio_offset - audio_tag_video_offset);
    const auto tagged_audio_section = audio_tag_answer_sdp.substr(audio_tag_audio_offset);
    require(untagged_video_section.find("m=video 40000 UDP/TLS/RTP/SAVPF 102\r\n") != std::string_view::npos &&
                untagged_video_section.find("a=bundle-only\r\n") == std::string_view::npos &&
                untagged_video_section.find("a=ice-ufrag:") == std::string_view::npos &&
                untagged_video_section.find("a=rtcp-mux\r\n") != std::string_view::npos &&
                tagged_audio_section.find("m=audio 40000 UDP/TLS/RTP/SAVPF 111\r\n") != std::string_view::npos &&
                tagged_audio_section.find("a=bundle-only\r\n") == std::string_view::npos &&
                tagged_audio_section.find("a=ice-ufrag:serverufrag\r\n") != std::string_view::npos &&
                tagged_audio_section.find("a=rtcp-mux\r\n") != std::string_view::npos,
            "webrtc transport follows answer bundle tag");

    auto rejected_audio_sdp = webrtc_offer_sdp;
    const auto opus_rtpmap = rejected_audio_sdp.find("a=rtpmap:111 opus/48000/2\r\n");
    require(opus_rtpmap != std::string::npos, "webrtc rejected audio source");
    rejected_audio_sdp.replace(opus_rtpmap, std::string_view("a=rtpmap:111 opus/48000/2\r\n").size(), "a=rtpmap:111 ISAC/48000/2\r\n");
    const auto rejected_audio_offer = parse_webrtc_offer(rejected_audio_sdp);
    require(rejected_audio_offer.has_value(), "webrtc parse rejected audio offer");
    const auto rejected_audio_answer = make_webrtc_answer(*rejected_audio_offer, tracks, config);
    require(rejected_audio_answer.has_value() && rejected_audio_answer->transport_mid == "0" && rejected_audio_answer->video_payload_type == 102 &&
                !rejected_audio_answer->audio_payload_type.has_value(),
            "webrtc reject non tagged unsupported audio");
    require(rejected_audio_answer->sdp.find("a=group:BUNDLE 0\r\n") != std::string::npos &&
                rejected_audio_answer->sdp.find("m=audio 0 UDP/TLS/RTP/SAVPF 111 0 8\r\n") != std::string::npos,
            "webrtc rejected audio keeps slot and leaves bundle");
    const auto rejected_audio_offset = rejected_audio_answer->sdp.find("m=audio 0 ");
    require(rejected_audio_offset != std::string::npos &&
                rejected_audio_answer->sdp.find("a=bundle-only\r\n", rejected_audio_offset) == std::string::npos &&
                rejected_audio_answer->sdp.find("a=inactive\r\n", rejected_audio_offset) == std::string::npos,
            "webrtc rejected audio minimal attributes");

    auto datachannel_sdp = webrtc_offer_sdp;
    const auto datachannel_bundle = datachannel_sdp.find(bundle);
    require(datachannel_bundle != std::string::npos, "webrtc datachannel bundle source");
    datachannel_sdp.replace(datachannel_bundle, bundle.size(), "a=group:BUNDLE 0 2 1\r\n");
    const auto datachannel_audio = datachannel_sdp.find("m=audio ");
    require(datachannel_audio != std::string::npos, "webrtc datachannel audio source");
    datachannel_sdp.insert(datachannel_audio,
                           "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n"
                           "c=IN IP4 0.0.0.0\r\n"
                           "a=mid:2\r\n"
                           "a=sctp-port:5000\r\n");
    const auto datachannel_offer = parse_webrtc_offer(datachannel_sdp);
    require(datachannel_offer.has_value() && datachannel_offer->media.size() == 3 &&
                datachannel_offer->media[1].formats == std::vector<std::string>{"webrtc-datachannel"},
            "webrtc preserve datachannel raw format");
    const auto datachannel_answer = make_webrtc_answer(*datachannel_offer, tracks, config);
    require(datachannel_answer.has_value() && datachannel_answer->sdp.find("a=group:BUNDLE 0 1\r\n") != std::string::npos,
            "webrtc rejected datachannel leaves bundle");
    const auto data_video_offset = datachannel_answer->sdp.find("m=video ");
    const auto data_application_offset = datachannel_answer->sdp.find("m=application ");
    const auto data_audio_offset = datachannel_answer->sdp.find("m=audio ");
    require(data_video_offset != std::string::npos && data_application_offset != std::string::npos && data_audio_offset != std::string::npos &&
                data_video_offset < data_application_offset && data_application_offset < data_audio_offset,
            "webrtc rejected datachannel preserves media order");
    const auto application_section = datachannel_answer->sdp.substr(data_application_offset, data_audio_offset - data_application_offset);
    require(application_section.find("m=application 0 UDP/DTLS/SCTP webrtc-datachannel\r\n") != std::string_view::npos &&
                application_section.find("c=IN IP4 0.0.0.0\r\n") != std::string_view::npos &&
                application_section.find("a=mid:2\r\n") != std::string_view::npos &&
                application_section.find("a=bundle-only\r\n") == std::string_view::npos &&
                application_section.find("a=inactive\r\n") == std::string_view::npos,
            "webrtc rejected datachannel raw format");

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
    require(partial_answer->sdp.find("m=audio 0 UDP/TLS/RTP/SAVPF") != std::string::npos, "webrtc unbundled audio rejected");

    auto missing_mid_extension_sdp = webrtc_offer_sdp;
    const std::string mid_extension = "a=extmap:4 urn:ietf:params:rtp-hdrext:sdes:mid\r\n";
    const auto missing_mid_extension_offset = missing_mid_extension_sdp.find(mid_extension);
    require(missing_mid_extension_offset != std::string::npos, "webrtc mid extension offer");
    missing_mid_extension_sdp.erase(missing_mid_extension_offset, mid_extension.size());
    const auto missing_mid_extension = parse_webrtc_offer(missing_mid_extension_sdp);
    require(missing_mid_extension.has_value(), "webrtc parse missing video mid extension");
    const auto missing_mid_extension_answer = make_webrtc_answer(*missing_mid_extension, tracks, config);
    require(!missing_mid_extension_answer.has_value(), "webrtc reject offer without mid extension on bundle tag");

    auto mismatched_mid_extension_sdp = webrtc_offer_sdp;
    const auto first_mid_extension_offset = mismatched_mid_extension_sdp.find(mid_extension);
    const auto second_mid_extension_offset = mismatched_mid_extension_sdp.find(
        mid_extension, first_mid_extension_offset == std::string::npos ? 0 : first_mid_extension_offset + mid_extension.size());
    require(first_mid_extension_offset != std::string::npos && second_mid_extension_offset != std::string::npos, "webrtc bundled mid extension ids");
    mismatched_mid_extension_sdp.replace(second_mid_extension_offset, mid_extension.size(), "a=extmap:5 urn:ietf:params:rtp-hdrext:sdes:mid\r\n");
    const auto mismatched_mid_extension = parse_webrtc_offer(mismatched_mid_extension_sdp);
    require(mismatched_mid_extension.has_value(), "webrtc parse mismatched mid extension ids");
    const auto mismatched_mid_extension_answer = make_webrtc_answer(*mismatched_mid_extension, tracks, config);
    require(mismatched_mid_extension_answer.has_value() && mismatched_mid_extension_answer->video_payload_type == 102 &&
                !mismatched_mid_extension_answer->audio_payload_type.has_value(),
            "webrtc reject bundled media with different mid extension id");

    auto reused_payload_type_sdp = webrtc_offer_sdp;
    const auto audio_media_offset = reused_payload_type_sdp.find("m=audio 9 UDP/TLS/RTP/SAVPF 111 0 8\r\n");
    require(audio_media_offset != std::string::npos, "webrtc bundled payload type source");
    reused_payload_type_sdp.replace(
        audio_media_offset, std::string_view("m=audio 9 UDP/TLS/RTP/SAVPF 111 0 8\r\n").size(), "m=audio 9 UDP/TLS/RTP/SAVPF 102 0 8\r\n");
    const auto opus_rtpmap_offset = reused_payload_type_sdp.find("a=rtpmap:111 opus/48000/2\r\n", audio_media_offset);
    const auto opus_fmtp_offset = reused_payload_type_sdp.find("a=fmtp:111 minptime=10;useinbandfec=1;stereo=1\r\n", audio_media_offset);
    require(opus_rtpmap_offset != std::string::npos && opus_fmtp_offset != std::string::npos, "webrtc bundled opus payload source");
    reused_payload_type_sdp.replace(opus_rtpmap_offset, std::string_view("a=rtpmap:111 opus/48000/2\r\n").size(), "a=rtpmap:102 opus/48000/2\r\n");
    reused_payload_type_sdp.replace(opus_fmtp_offset,
                                    std::string_view("a=fmtp:111 minptime=10;useinbandfec=1;stereo=1\r\n").size(),
                                    "a=fmtp:102 minptime=10;useinbandfec=1;stereo=1\r\n");
    const auto reused_payload_type = parse_webrtc_offer(reused_payload_type_sdp);
    require(reused_payload_type.has_value(), "webrtc parse bundled reused payload type");
    const auto reused_payload_type_answer = make_webrtc_answer(*reused_payload_type, tracks, config);
    require(reused_payload_type_answer.has_value() && reused_payload_type_answer->video_payload_type == 102 &&
                !reused_payload_type_answer->audio_payload_type.has_value(),
            "webrtc reject conflicting bundled payload type reuse");

    auto long_mid_sdp = webrtc_offer_sdp;
    const std::string video_mid = "a=mid:0\r\n";
    const auto video_mid_offset = long_mid_sdp.find(video_mid);
    require(video_mid_offset != std::string::npos, "webrtc mid length offer");
    long_mid_sdp.replace(video_mid_offset, video_mid.size(), "a=mid:0123456789abcdef0\r\n");
    require(!parse_webrtc_offer(long_mid_sdp).has_value(), "webrtc reject mid longer than 16 characters");

    auto unknown_bundle_mid_sdp = webrtc_offer_sdp;
    const auto unknown_bundle_offset = unknown_bundle_mid_sdp.find(bundle);
    require(unknown_bundle_offset != std::string::npos, "webrtc bundle mid offer");
    unknown_bundle_mid_sdp.replace(unknown_bundle_offset, bundle.size(), "a=group:BUNDLE 0 missing\r\n");
    require(!parse_webrtc_offer(unknown_bundle_mid_sdp).has_value(), "webrtc reject unknown bundle mid");
}

void test_whep_session_startup_errors()
{
    boost::asio::io_context io;
    auto stream = std::make_shared<media_stream>("live/startup-errors", io.get_executor());
    require(stream->set_tracks({make_video_track(), make_audio_track()}), "initial tracks");

    const auto offer = parse_webrtc_offer(webrtc_offer_sdp);
    require(offer.has_value(), "startup errors parse offer");
    auto certificate = dtls_certificate::create();
    require(certificate != nullptr, "startup errors certificate");

    const auto make_session = [&](std::shared_ptr<media_stream> source, std::shared_ptr<dtls_certificate> session_certificate)
    {
        return std::make_shared<whep_session>(
            io.get_executor(), std::move(source), boost::asio::ip::make_address("127.0.0.1"), std::move(session_certificate));
    };

    auto invalid_offer = *offer;
    require(!invalid_offer.media.empty(), "startup errors media");
    invalid_offer.media.front().ice_ufrag.clear();
    require(make_session(stream, certificate)->startup(std::move(invalid_offer)) == whep_session_startup_error::invalid_offer,
            "startup errors invalid offer");

    auto invalid_fingerprint_offer = *offer;
    invalid_fingerprint_offer.media.front().fingerprint = "invalid";
    require(make_session(stream, certificate)->startup(std::move(invalid_fingerprint_offer)) == whep_session_startup_error::invalid_offer,
            "startup errors invalid fingerprint");

    auto rejected_tag_offer = *offer;
    require(!rejected_tag_offer.media.front().codecs.empty(), "startup errors tagged codec");
    rejected_tag_offer.media.front().codecs.front().encoding_name = "VP8";
    require(make_session(stream, certificate)->startup(std::move(rejected_tag_offer)) == whep_session_startup_error::invalid_offer,
            "startup errors rejected bundle tag");

    require(make_session(stream, nullptr)->startup(*offer) == whep_session_startup_error::internal_error, "startup errors internal error");
}

void test_whep_session_lifecycle()
{
    boost::asio::io_context io;
    auto& streams = registry::instance();
    streams.clear();
    auto stream = std::make_shared<media_stream>("live/test", io.get_executor());
    require(stream->set_tracks({make_video_track(), make_audio_track()}), "initial tracks");
    require(streams.add(stream), "whep registry add");

    const config application_config;
    auto missing_ice_offer = webrtc_offer_sdp;
    const std::string video_ice_ufrag = "a=ice-ufrag:remotevideo\r\n";
    const auto video_ice_offset = missing_ice_offer.find(video_ice_ufrag);
    require(video_ice_offset != std::string::npos, "whep invalid offer ice attribute");
    missing_ice_offer.erase(video_ice_offset, video_ice_ufrag.size());
    require(whep::create(io.get_executor(), "live/test", missing_ice_offer, application_config).error == whep::create_error::invalid_offer, "whep semantic invalid offer");

    const auto first = whep::create(io.get_executor(), "live/test", webrtc_offer_sdp, application_config);
    const auto second = whep::create(io.get_executor(), "live/test", webrtc_offer_sdp, application_config);
    require(first.error == whep::create_error::none && second.error == whep::create_error::none, "whep create multiple sessions");
    require(!first.session_id.empty() && !second.session_id.empty(), "whep session ids");
    require(first.session_id != second.session_id, "whep unique session ids");
    require(first.answer_sdp.find("a=ice-lite\r\n") != std::string::npos, "whep answer sdp");
    require(first.answer_sdp.find("a=candidate:1 1 UDP 2130706431 127.0.0.1 ") != std::string::npos, "whep host candidate");
    require(sdp_attribute(first.answer_sdp, "ice-ufrag") != sdp_attribute(second.answer_sdp, "ice-ufrag"), "whep unique ice ufrag");
    require(sdp_attribute(first.answer_sdp, "ice-pwd") != sdp_attribute(second.answer_sdp, "ice-pwd"), "whep unique ice password");
    require(sdp_attribute(first.answer_sdp, "fingerprint") != sdp_attribute(second.answer_sdp, "fingerprint"), "whep unique dtls fingerprint");

    require(whep::remove(first.session_id), "whep remove first session");
    drain_io(io);
    require(!whep::remove(first.session_id), "whep remove first once");
    require(whep::remove(second.session_id), "whep remove second session");
    drain_io(io);

    const auto third = whep::create(io.get_executor(), "live/test", webrtc_offer_sdp, application_config);
    require(third.error == whep::create_error::none, "whep recreate viewer");

    streams.remove(*stream);
    stream->end();
    drain_io(io);
    require(!whep::remove(third.session_id), "whep source end releases session");

    auto replacement = std::make_shared<media_stream>("live/test", io.get_executor());
    require(replacement->set_tracks({make_video_track(), make_audio_track()}), "initial tracks");
    require(streams.add(replacement), "whep replacement registry add");

    const auto replacement_session = whep::create(io.get_executor(), "live/test", webrtc_offer_sdp, application_config);
    require(replacement_session.error == whep::create_error::none, "whep create after republish");
    require(replacement_session.session_id != third.session_id, "whep republish new session id");

    auto updated_video = make_video_track();
    updated_video.codec_config.push_back(0x01);
    require(replacement->update_track(std::move(updated_video)), "whep source config update");
    drain_io(io);
    require(!whep::contains(replacement_session.session_id), "whep source config change releases session resource");
    require(!whep::remove(replacement_session.session_id), "whep source config change releases session");

    const auto updated_session = whep::create(io.get_executor(), "live/test", webrtc_offer_sdp, application_config);
    require(updated_session.error == whep::create_error::none, "whep create after config change");
    require(whep::remove(updated_session.session_id), "whep remove updated session");
    drain_io(io);
}

void test_whep_opus_source_session_lifecycle()
{
    boost::asio::io_context io;
    auto& streams = registry::instance();
    streams.clear();
    auto stream = std::make_shared<media_stream>("live/opus", io.get_executor());
    require(stream->set_tracks({make_video_track(), make_opus_track(1)}), "whep opus source tracks");
    require(streams.add(stream), "whep opus source registry add");

    const config application_config;
    auto compatible_sdp = webrtc_offer_sdp;
    const auto fmtp = compatible_sdp.find("a=fmtp:111 minptime=10;useinbandfec=1;stereo=1\r\n");
    require(fmtp != std::string::npos, "whep opus source compatible fmtp");
    compatible_sdp.replace(fmtp,
                           std::string_view("a=fmtp:111 minptime=10;useinbandfec=1;stereo=1\r\n").size(),
                           "a=fmtp:111 minptime=10;useinbandfec=1;stereo=1;maxaveragebitrate=510000\r\n");
    const auto session = whep::create(io.get_executor(), "live/opus", compatible_sdp, application_config);
    require(session.error == whep::create_error::none && session.answer_sdp.find("a=rtpmap:111 opus/48000/2\r\n") != std::string::npos &&
                session.answer_sdp.find("sprop-stereo=0") != std::string::npos,
            "whep opus source session answer");

    auto changed = make_opus_track(2);
    require(stream->update_track(std::move(changed)), "whep opus source track update");
    drain_io(io);
    require(!whep::remove(session.session_id), "whep opus source negotiated track lifecycle");
}

void test_whep_negotiated_track_lifecycle()
{
    boost::asio::io_context io;
    auto stream = std::make_shared<media_stream>("live/negotiated-tracks", io.get_executor());
    require(stream->set_tracks({make_h265_track(), make_audio_track()}), "initial tracks");

    auto certificate = dtls_certificate::create();
    require(certificate != nullptr, "negotiated tracks certificate");

    auto video_only_sdp = make_h265_offer(webrtc_offer_sdp);
    const std::string bundle = "a=group:BUNDLE 0 1\r\n";
    const auto video_bundle_offset = video_only_sdp.find(bundle);
    require(video_bundle_offset != std::string::npos, "negotiated tracks video bundle");
    video_only_sdp.replace(video_bundle_offset, bundle.size(), "a=group:BUNDLE 0\r\n");
    const auto video_only_offer = parse_webrtc_offer(video_only_sdp);
    require(video_only_offer.has_value(), "negotiated tracks video offer");

    auto video_session = std::make_shared<whep_session>(io.get_executor(), stream, boost::asio::ip::make_address("127.0.0.1"), certificate);
    require(video_session->startup(*video_only_offer) == whep_session_startup_error::none, "negotiated tracks video session");
    require(video_session->answer_sdp().find("a=group:BUNDLE 0\r\n") != std::string::npos, "negotiated tracks video answer");

    auto updated_audio = make_audio_track();
    updated_audio.clock_rate = 48'000;
    updated_audio.codec_config = {0x11, 0x90};
    require(stream->update_track(std::move(updated_audio)), "negotiated tracks unselected audio update");
    drain_io(io);
    require(video_session->local_port() != 0, "unselected audio keeps video session");

    auto updated_video = make_h265_track();
    updated_video.codec_config.push_back(0x01);
    require(stream->update_track(std::move(updated_video)), "negotiated tracks selected video update");
    drain_io(io);
    require(video_session->local_port() == 0, "selected video closes video session");

    auto audio_only_sdp = webrtc_offer_sdp;
    const auto audio_bundle_offset = audio_only_sdp.find(bundle);
    require(audio_bundle_offset != std::string::npos, "negotiated tracks audio bundle");
    audio_only_sdp.replace(audio_bundle_offset, bundle.size(), "a=group:BUNDLE 1\r\n");
    const auto audio_only_offer = parse_webrtc_offer(audio_only_sdp);
    require(audio_only_offer.has_value(), "negotiated tracks audio offer");

    auto audio_session = std::make_shared<whep_session>(io.get_executor(), stream, boost::asio::ip::make_address("127.0.0.1"), certificate);
    require(audio_session->startup(*audio_only_offer) == whep_session_startup_error::none, "negotiated tracks audio session");
    require(audio_session->answer_sdp().find("a=group:BUNDLE 1\r\n") != std::string::npos, "negotiated tracks audio answer");

    auto ignored_video = make_h265_track();
    ignored_video.codec_config.push_back(0x02);
    require(stream->update_track(std::move(ignored_video)), "negotiated tracks unselected video update");
    drain_io(io);
    require(audio_session->local_port() != 0, "unselected video keeps audio session");

    auto selected_audio = make_audio_track();
    selected_audio.clock_rate = 32'000;
    selected_audio.codec_config = {0x12, 0x90};
    require(stream->update_track(std::move(selected_audio)), "negotiated tracks selected audio update");
    drain_io(io);
    require(audio_session->local_port() == 0, "selected audio closes audio session");
}

void test_whep_self_owned_lifecycle()
{
    boost::asio::io_context io;
    auto stream = std::make_shared<media_stream>("live/self-owned", io.get_executor());
    require(stream->set_tracks({make_video_track()}), "self owned video track");

    const auto offer = parse_webrtc_offer(webrtc_offer_sdp);
    require(offer.has_value(), "self owned parse offer");
    auto certificate = dtls_certificate::create();
    require(certificate != nullptr, "self owned certificate");

    auto session = std::make_shared<whep_session>(io.get_executor(), stream, boost::asio::ip::make_address("127.0.0.1"), certificate);
    require(session->startup(*offer) == whep_session_startup_error::none, "self owned session startup");
    const std::weak_ptr<whep_session> weak_session = session;
    session.reset();
    require(!weak_session.expired(), "self owned session kept by async receive");

    stream->end();
    drain_io(io);
    require(weak_session.expired(), "self owned session released after safe shutdown");
}

void test_whep_multi_session_isolation()
{
    boost::asio::io_context io;
    auto stream = std::make_shared<media_stream>("live/multi", io.get_executor());
    require(stream->set_tracks({make_video_track(), make_audio_track()}), "initial tracks");

    const auto offer = parse_webrtc_offer(webrtc_offer_sdp);
    require(offer.has_value(), "multi parse offer");
    auto certificate = dtls_certificate::create();
    require(certificate != nullptr, "multi certificate");

    auto first = std::make_shared<whep_session>(io.get_executor(), stream, boost::asio::ip::make_address("127.0.0.1"), certificate);
    auto second = std::make_shared<whep_session>(io.get_executor(), stream, boost::asio::ip::make_address("127.0.0.1"), certificate);
    require(first->startup(*offer) == whep_session_startup_error::none && second->startup(*offer) == whep_session_startup_error::none,
            "multi sessions startup");
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

    first->shutdown();
    drain_io(io);
    require(first->local_port() == 0 && !first->ice_connected(), "multi first closed");
    require(second->local_port() != 0 && second->ice_connected(), "multi second remains connected");

    const std::array<std::uint8_t, 12> keepalive_id{3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3};
    require_stun_success(
        exchange_stun(io, second_client, second_endpoint, make_stun_request(second_ufrag + ":remotevideo", second_pwd, keepalive_id, false)),
        keepalive_id);
    require(second->ice_connected(), "multi second survives first shutdown");

    stream->end();
    drain_io(io);
    require(second->local_port() == 0, "multi source end closes second");
    require(!second->ice_connected(), "multi source end clears ice");

    boost::system::error_code error;
    first_client.close(error);
    second_client.close(error);
}

void test_whep_establishment_timeout()
{
    boost::asio::io_context io;
    auto stream = std::make_shared<media_stream>("live/establishment-timeout", io.get_executor());
    require(stream->set_tracks({make_video_track(), make_audio_track()}), "initial tracks");

    const auto offer = parse_webrtc_offer(webrtc_offer_sdp);
    require(offer.has_value(), "establishment timeout parse offer");
    auto certificate = dtls_certificate::create();
    require(certificate != nullptr, "establishment timeout certificate");

    auto session = std::make_shared<whep_session>(io.get_executor(),
                                                  stream,
                                                  boost::asio::ip::make_address("127.0.0.1"),
                                                  certificate,
                                                  whep_session_timeouts{
                                                      .establishment = std::chrono::milliseconds(20),
                                                      .ice_activity = std::chrono::seconds(1),
                                                  });
    require(session->startup(*offer) == whep_session_startup_error::none, "establishment timeout session startup");
    require(session->local_port() != 0, "establishment timeout socket open");

    io.run_for(std::chrono::milliseconds(80));
    io.restart();

    require(session->local_port() == 0, "establishment timeout closes socket");
    require(!session->ice_connected(), "establishment timeout clears ice");

    session->shutdown();
    drain_io(io);
    require(session->local_port() == 0, "establishment timeout repeated shutdown ignored");
}

void test_whep_ice_activity_timeout()
{
    boost::asio::io_context io;
    auto stream = std::make_shared<media_stream>("live/ice-activity-timeout", io.get_executor());
    require(stream->set_tracks({make_video_track(), make_audio_track()}), "initial tracks");

    const auto offer = parse_webrtc_offer(webrtc_offer_sdp);
    require(offer.has_value(), "ice activity timeout parse offer");
    auto certificate = dtls_certificate::create();
    require(certificate != nullptr, "ice activity timeout certificate");

    auto session = std::make_shared<whep_session>(io.get_executor(),
                                                  stream,
                                                  boost::asio::ip::make_address("127.0.0.1"),
                                                  certificate,
                                                  whep_session_timeouts{
                                                      .establishment = std::chrono::seconds(1),
                                                      .ice_activity = std::chrono::milliseconds(80),
                                                  });
    require(session->startup(*offer) == whep_session_startup_error::none, "ice activity timeout session startup");

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

    boost::system::error_code error;
    client.close(error);
}

void test_stun_ice_connectivity_check_contract()
{
    constexpr std::string_view username = "local:remote";
    constexpr std::string_view password = "localpassword1234567890";
    const std::array<std::uint8_t, 12> transaction_id{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};

    const auto valid = make_stun_request(username, password, transaction_id, true);
    const auto parsed = parse_stun_binding_request(valid, username, password);
    require(parsed.has_value() && parsed->priority && parsed->use_candidate && parsed->ice_controlling && !parsed->ice_controlled,
            "stun valid ice connectivity check");

    const auto missing_priority = parse_stun_binding_request(
        make_stun_request(username, password, transaction_id, false, stun_request_variant::missing_priority), username, password);
    require(missing_priority.has_value() && !missing_priority->priority, "stun parse missing priority");

    const auto missing_ice_controlling = parse_stun_binding_request(
        make_stun_request(username, password, transaction_id, false, stun_request_variant::missing_ice_controlling), username, password);
    require(missing_ice_controlling.has_value() && !missing_ice_controlling->ice_controlling && !missing_ice_controlling->ice_controlled,
            "stun parse missing ice role");

    const auto ice_controlled = parse_stun_binding_request(
        make_stun_request(username, password, transaction_id, false, stun_request_variant::ice_controlled), username, password);
    require(ice_controlled.has_value() && ice_controlled->ice_controlled && !ice_controlled->ice_controlling, "stun parse ice controlled peer");

    require(!parse_stun_binding_request(
                 make_stun_request(username, password, transaction_id, false, stun_request_variant::missing_fingerprint), username, password)
                 .has_value(),
            "stun reject missing fingerprint");
    const auto use_candidate_after_integrity = parse_stun_binding_request(
        make_stun_request(username, password, transaction_id, true, stun_request_variant::use_candidate_after_integrity), username, password);
    require(use_candidate_after_integrity.has_value() && !use_candidate_after_integrity->use_candidate && use_candidate_after_integrity->priority &&
                use_candidate_after_integrity->ice_controlling,
            "stun ignore use candidate after message integrity");

    constexpr std::array<std::uint16_t, 2> required_attributes{0x1234, 0x2345};
    const auto unknown_required = parse_stun_binding_request(
        make_stun_request(username, password, transaction_id, false, stun_request_variant::valid, required_attributes), username, password);
    require(unknown_required.has_value() &&
                unknown_required->unknown_required_attributes == std::vector<std::uint16_t>(required_attributes.begin(), required_attributes.end()),
            "stun collect unknown required attributes");

    constexpr std::array<std::uint16_t, 1> optional_attributes{0x9234};
    const auto unknown_optional = parse_stun_binding_request(
        make_stun_request(username, password, transaction_id, false, stun_request_variant::valid, optional_attributes), username, password);
    require(unknown_optional.has_value() && unknown_optional->unknown_required_attributes.empty(), "stun ignore unknown optional attribute");

    const auto after_integrity = parse_stun_binding_request(
        make_stun_request(username, password, transaction_id, false, stun_request_variant::attributes_after_integrity, required_attributes),
        username,
        password);
    require(after_integrity.has_value() && after_integrity->unknown_required_attributes.empty(), "stun ignore attributes after message integrity");
}

void test_whep_stun_unknown_attribute_contract()
{
    boost::asio::io_context io;
    auto stream = std::make_shared<media_stream>("live/stun-unknown", io.get_executor());
    require(stream->set_tracks({make_video_track(), make_audio_track()}), "stun unknown initial tracks");

    const auto offer = parse_webrtc_offer(webrtc_offer_sdp);
    require(offer.has_value(), "stun unknown parse offer");
    auto certificate = dtls_certificate::create();
    require(certificate != nullptr, "stun unknown certificate");

    auto session = std::make_shared<whep_session>(io.get_executor(), stream, boost::asio::ip::make_address("127.0.0.1"), certificate);
    require(session->startup(*offer) == whep_session_startup_error::none, "stun unknown session startup");

    const auto local_ufrag = sdp_attribute(session->answer_sdp(), "ice-ufrag");
    const auto local_pwd = sdp_attribute(session->answer_sdp(), "ice-pwd");
    require(!local_ufrag.empty() && !local_pwd.empty(), "stun unknown local credentials");

    boost::asio::ip::udp::socket client(io, boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v4(), 0));
    const boost::asio::ip::udp::endpoint server_endpoint(boost::asio::ip::make_address("127.0.0.1"), session->local_port());
    const auto username = local_ufrag + ":remotevideo";

    constexpr std::array<std::uint16_t, 1> single_unknown{0x1234};
    const std::array<std::uint8_t, 12> single_id{1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
    const auto single_response = exchange_stun(
        io, client, server_endpoint, make_stun_request(username, local_pwd, single_id, true, stun_request_variant::valid, single_unknown));
    require_stun_error(single_response, single_id, 420, single_unknown, local_pwd);
    require(!session->ice_connected(), "stun unknown does not nominate");

    constexpr std::array<std::uint16_t, 2> multiple_unknown{0x1234, 0x2345};
    const std::array<std::uint8_t, 12> multiple_id{2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2};
    const auto multiple_response = exchange_stun(
        io, client, server_endpoint, make_stun_request(username, local_pwd, multiple_id, false, stun_request_variant::valid, multiple_unknown));
    require_stun_error(multiple_response, multiple_id, 420, multiple_unknown, local_pwd);

    constexpr std::array<std::uint16_t, 1> optional_unknown{0x9234};
    const std::array<std::uint8_t, 12> optional_id{3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3};
    require_stun_success(
        exchange_stun(
            io, client, server_endpoint, make_stun_request(username, local_pwd, optional_id, false, stun_request_variant::valid, optional_unknown)),
        optional_id);
    require(!session->ice_connected(), "stun optional unknown does not nominate");

    const std::array<std::uint8_t, 12> role_conflict_id{4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4};
    const auto role_conflict_response =
        exchange_stun(io,
                      client,
                      server_endpoint,
                      make_stun_request(username, local_pwd, role_conflict_id, false, stun_request_variant::ice_controlled, single_unknown));
    require_stun_error(role_conflict_response, role_conflict_id, 420, single_unknown, local_pwd);

    const std::array<std::uint8_t, 12> unauthenticated_id{5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5};
    const auto unauthenticated_request =
        make_stun_request(username, "wrongpassword1234567890", unauthenticated_id, false, stun_request_variant::valid, single_unknown);
    static_cast<void>(client.send_to(boost::asio::buffer(unauthenticated_request), server_endpoint));
    io.run_for(std::chrono::milliseconds(100));
    io.restart();
    boost::system::error_code available_error;
    require(client.available(available_error) == 0U && !available_error, "stun unknown unauthenticated request has no response");

    session->shutdown();
    drain_io(io);
    boost::system::error_code error;
    client.close(error);
}

void test_whep_ice_lite()
{
    boost::asio::io_context io;
    auto stream = std::make_shared<media_stream>("live/ice", io.get_executor());
    require(stream->set_tracks({make_video_track(), make_audio_track()}), "initial tracks");

    const auto offer = parse_webrtc_offer(webrtc_offer_sdp);
    require(offer.has_value(), "ice parse offer");
    auto certificate = dtls_certificate::create();
    require(certificate != nullptr, "ice certificate");

    auto session = std::make_shared<whep_session>(io.get_executor(), stream, boost::asio::ip::make_address("127.0.0.1"), certificate);
    require(session->startup(*offer) == whep_session_startup_error::none, "ice session startup");

    const auto local_ufrag = sdp_attribute(session->answer_sdp(), "ice-ufrag");
    const auto local_pwd = sdp_attribute(session->answer_sdp(), "ice-pwd");
    require(!local_ufrag.empty() && !local_pwd.empty(), "ice local credentials");

    boost::asio::ip::udp::socket client(io, boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v4(), 0));
    const boost::asio::ip::udp::endpoint server_endpoint(boost::asio::ip::make_address("127.0.0.1"), session->local_port());
    const auto username = local_ufrag + ":remotevideo";

    const std::array<std::uint8_t, 12> role_conflict_id{12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1};
    const auto role_conflict_response = exchange_stun(
        io, client, server_endpoint, make_stun_request(username, local_pwd, role_conflict_id, true, stun_request_variant::ice_controlled));
    require_stun_error(role_conflict_response, role_conflict_id, 487, {}, local_pwd);
    require(!session->ice_connected(), "ice role conflict not nominated");

    const std::array<std::uint8_t, 12> after_integrity_id{1, 3, 5, 7, 9, 11, 2, 4, 6, 8, 10, 12};
    const auto after_integrity_response =
        exchange_stun(io,
                      client,
                      server_endpoint,
                      make_stun_request(username, local_pwd, after_integrity_id, true, stun_request_variant::use_candidate_after_integrity));
    require_stun_success(after_integrity_response, after_integrity_id);
    require(!session->ice_connected(), "ice use candidate after message integrity ignored");

    const std::array<std::uint8_t, 12> check_id{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    const auto check_response = exchange_stun(io, client, server_endpoint, make_stun_request(username, local_pwd, check_id, false));
    require_stun_success(check_response, check_id);
    require(!session->ice_connected(), "ice check not nominated");

    const std::array<std::uint8_t, 12> nominate_id{11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0};
    const auto nominate_response = exchange_stun(io, client, server_endpoint, make_stun_request(username, local_pwd, nominate_id, true));
    require_stun_success(nominate_response, nominate_id);
    require(session->ice_connected(), "ice nominated");

    session->shutdown();
    drain_io(io);
    boost::system::error_code error;
    client.close(error);
}

void test_whep_selected_bundle_transport()
{
    boost::asio::io_context io;
    auto certificate = dtls_certificate::create();
    require(certificate != nullptr, "selected transport certificate");
    const auto check = [&io, &certificate](std::vector<media_track> tracks, const std::string& sdp, std::string_view remote_ufrag, std::uint8_t id)
    {
        auto stream = std::make_shared<media_stream>("live/selected-transport", io.get_executor());
        require(stream->set_tracks(std::move(tracks)), "selected transport tracks");
        const auto offer = parse_webrtc_offer(sdp);
        require(offer.has_value(), "selected transport offer");
        auto session = std::make_shared<whep_session>(io.get_executor(), stream, boost::asio::ip::make_address("127.0.0.1"), certificate);
        require(session->startup(*offer) == whep_session_startup_error::none, "selected transport session startup");

        const auto local_ufrag = sdp_attribute(session->answer_sdp(), "ice-ufrag");
        const auto local_pwd = sdp_attribute(session->answer_sdp(), "ice-pwd");
        boost::asio::ip::udp::socket client(io, boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v4(), 0));
        const boost::asio::ip::udp::endpoint endpoint(boost::asio::ip::make_address("127.0.0.1"), session->local_port());
        std::array<std::uint8_t, 12> transaction_id{};
        transaction_id.fill(id);
        require_stun_success(
            exchange_stun(io, client, endpoint, make_stun_request(local_ufrag + ":" + std::string(remote_ufrag), local_pwd, transaction_id, true)),
            transaction_id);
        session->shutdown();
        drain_io(io);
    };

    check({make_video_track(), make_audio_track()}, webrtc_offer_sdp, "remotevideo", 6);
    check({make_video_track(), make_audio_track()}, make_audio_tag_offer(webrtc_offer_sdp), "remoteaudio", 7);
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

void test_whep_dtls(codec_id video_codec, const char* srtp_profile, bool server_shutdown)
{
    require(video_codec == codec_id::h264 || video_codec == codec_id::h265, "dtls video codec");
    const bool h265 = video_codec == codec_id::h265;
    boost::asio::io_context io;
    auto stream = std::make_shared<media_stream>("live/dtls", io.get_executor());
    require(stream->set_tracks({h265 ? make_h265_track() : make_video_track(), make_audio_track()}), "initial tracks");

    auto server_certificate = dtls_certificate::create();
    auto client_certificate = dtls_certificate::create();
    require(server_certificate != nullptr && client_certificate != nullptr, "dtls certificates");
    require(EVP_PKEY_base_id(server_certificate->private_key()) == EVP_PKEY_EC && EVP_PKEY_bits(server_certificate->private_key()) == 256,
            "dtls server certificate ecdsa p256");
    require(EVP_PKEY_base_id(client_certificate->private_key()) == EVP_PKEY_EC && EVP_PKEY_bits(client_certificate->private_key()) == 256,
            "dtls client certificate ecdsa p256");

    auto offer_sdp = offer_with_fingerprint(client_certificate->sha256_fingerprint());
    if (h265)
    {
        offer_sdp = make_h265_offer(std::move(offer_sdp));
    }
    const auto offer = parse_webrtc_offer(offer_sdp);
    require(offer.has_value(), "dtls parse offer");

    auto session = std::make_shared<whep_session>(io.get_executor(), stream, boost::asio::ip::make_address("127.0.0.1"), server_certificate);
    require(session->startup(*offer) == whep_session_startup_error::none, "dtls session startup");
    require(sdp_attribute(session->answer_sdp(), "fingerprint") == "sha-256 " + server_certificate->sha256_fingerprint(),
            "dtls answer server fingerprint");

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

    boost::asio::ip::udp::socket second_client_socket(io, boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v4(), 0));
    const std::array<std::uint8_t, 12> second_nominate_id{2, 1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    const auto second_nominate = make_stun_request(local_ufrag + ":remotevideo", local_pwd, second_nominate_id, true);
    static_cast<void>(second_client_socket.send_to(boost::asio::buffer(second_nominate), server_endpoint));
    drain_io(io);

    auto client = make_dtls_test_client(client_certificate, srtp_profile);
    require(client.has_value(), "dtls client create");
    require(drive_dtls_client(io, client_socket, server_endpoint, *session, *client), "dtls handshake");
    require(session->dtls_connected(), "dtls server connected");
    require(std::string_view(SSL_get_cipher_name(client->ssl.get())) == "ECDHE-ECDSA-AES128-GCM-SHA256", "dtls mandatory webrtc cipher");
    const auto* selected_srtp_profile = SSL_get_selected_srtp_profile(client->ssl.get());
    require(
        selected_srtp_profile != nullptr && selected_srtp_profile->name != nullptr && std::string_view(selected_srtp_profile->name) == srtp_profile,
        "dtls srtp profile");
    std::unique_ptr<X509, decltype(&X509_free)> peer_certificate(SSL_get1_peer_certificate(client->ssl.get()), &X509_free);
    require(peer_certificate != nullptr && X509_cmp(peer_certificate.get(), server_certificate->certificate()) == 0,
            "dtls server certificate matches answer");

    require(session->srtp_started(), "srtp server started");

    const auto peer_material = make_peer_srtp_material(client->ssl.get());
    require(peer_material.has_value(), "dtls client srtp material");
    srtp_transport peer_srtp;
    require(peer_srtp.startup(peer_material->outbound), "srtp peer startup");

    const std::array<std::uint8_t, 12> repeated_rtp{0x80, 96, 0x12, 0x34, 0, 0, 0, 1, 0x11, 0x22, 0x33, 0x44};
    require(peer_srtp.protect_rtp(repeated_rtp).has_value(), "srtp first protect");
    require(!peer_srtp.protect_rtp(repeated_rtp).has_value(), "srtp repeated sequence rejected");

    auto peer_receive_key = peer_material->inbound_key;
    peer_receive_key.insert(peer_receive_key.end(), peer_material->inbound_salt.begin(), peer_material->inbound_salt.end());
    srtp_policy_t peer_receive_policy{};
    if (peer_material->outbound.profile == "SRTP_AEAD_AES_128_GCM")
    {
        srtp_crypto_policy_set_aes_gcm_128_16_auth(&peer_receive_policy.rtp);
        srtp_crypto_policy_set_aes_gcm_128_16_auth(&peer_receive_policy.rtcp);
    }
    else if (peer_material->outbound.profile == "SRTP_AEAD_AES_256_GCM")
    {
        srtp_crypto_policy_set_aes_gcm_256_16_auth(&peer_receive_policy.rtp);
        srtp_crypto_policy_set_aes_gcm_256_16_auth(&peer_receive_policy.rtcp);
    }
    else
    {
        srtp_crypto_policy_set_rtp_default(&peer_receive_policy.rtp);
        srtp_crypto_policy_set_rtcp_default(&peer_receive_policy.rtcp);
    }
    peer_receive_policy.ssrc.type = ssrc_any_inbound;
    peer_receive_policy.key = peer_receive_key.data();
    peer_receive_policy.window_size = 1024;
    srtp_t peer_receiver{};
    require(srtp_create(&peer_receiver, &peer_receive_policy) == srtp_err_status_ok, "srtp peer receiver create");

    const auto unprotect_server_packet = [peer_receiver](std::span<const std::uint8_t> packet) -> std::optional<test_srtp_packet>
    {
        if (packet.empty() || packet.size() > static_cast<std::size_t>(INT_MAX))
        {
            return std::nullopt;
        }

        const bool rtcp = packet.size() >= 2U && packet[1] >= 192U && packet[1] <= 223U;
        std::vector<std::uint8_t> output(packet.begin(), packet.end());
        int size = static_cast<int>(output.size());
        const auto status = rtcp ? srtp_unprotect_rtcp(peer_receiver, output.data(), &size) : srtp_unprotect(peer_receiver, output.data(), &size);
        if (status != srtp_err_status_ok || size < 0)
        {
            return std::nullopt;
        }
        output.resize(static_cast<std::size_t>(size));
        return test_srtp_packet{.rtcp = rtcp, .bytes = std::move(output)};
    };

    stream->publish(make_video_key_frame(video_codec));

    std::array<std::uint8_t, 4096> rtp_buffer{};
    boost::asio::ip::udp::endpoint rtp_sender;
    std::optional<test_srtp_packet> clear_rtp;
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
        clear_rtp = unprotect_server_packet(packet);
    }

    require(clear_rtp.has_value() && !clear_rtp->rtcp, "srtp decrypt video rtp");
    require(clear_rtp->bytes.size() >= 14U, "srtp clear rtp header");
    require((clear_rtp->bytes[0] >> 6U) == 2U, "srtp clear rtp version");
    require((clear_rtp->bytes[1] & 0x7fU) == 102U, "srtp negotiated video payload");
    const auto video_payload = require_rtp_mid(clear_rtp->bytes, "0", 4);
    require(!video_payload.empty(), "srtp video payload");
    const auto nal_type = h265 ? ((video_payload[0] >> 1U) & 0x3fU) : (video_payload[0] & 0x1fU);
    require(nal_type == (h265 ? 19U : 5U), "srtp negotiated video codec");
    const auto video_ssrc = read_network_u32(clear_rtp->bytes, 8U);

    std::optional<test_srtp_packet> clear_server_rtcp;
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
        auto clear = unprotect_server_packet(packet);
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
        stream->publish(make_audio_frame(index, audio_pts_ns));
        audio_pts_ns += 23'219'954;
    }

    std::optional<test_srtp_packet> clear_audio_rtp;
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
        auto clear = unprotect_server_packet(packet);
        if (clear && !clear->rtcp && clear->bytes.size() >= 12U && (clear->bytes[1] & 0x7fU) == 111U)
        {
            clear_audio_rtp = std::move(clear);
        }
    }

    require(clear_audio_rtp.has_value(), "srtp decrypt opus rtp");
    require((clear_audio_rtp->bytes[0] >> 6U) == 2U, "srtp clear opus rtp version");
    require((clear_audio_rtp->bytes[1] & 0x7fU) == 111U, "srtp negotiated opus payload");
    require(!require_rtp_mid(clear_audio_rtp->bytes, "1", 4).empty(), "srtp opus mid payload");

    require(srtp_dealloc(peer_receiver) == srtp_err_status_ok, "srtp peer receiver destroy");

    if (server_shutdown)
    {
        session->shutdown();
        bool close_notify_received = false;
        const auto close_notify_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (!close_notify_received && std::chrono::steady_clock::now() < close_notify_deadline)
        {
            io.run_for(std::chrono::milliseconds(10));
            io.restart();

            boost::system::error_code receive_error;
            const auto size = client_socket.receive_from(boost::asio::buffer(rtp_buffer), rtp_sender, 0, receive_error);
            if (receive_error == boost::asio::error::would_block || receive_error == boost::asio::error::try_again)
            {
                continue;
            }
            require(!receive_error && rtp_sender == server_endpoint, "dtls server close notify receive");
            const auto packet = std::span<const std::uint8_t>(rtp_buffer.data(), size);
            if (!dtls_transport::is_dtls_packet(packet))
            {
                continue;
            }
            require(BIO_write(client->read_bio, packet.data(), static_cast<int>(packet.size())) == static_cast<int>(packet.size()),
                    "dtls server close notify input");
            std::array<std::uint8_t, 1> discarded{};
            const auto result = SSL_read(client->ssl.get(), discarded.data(), static_cast<int>(discarded.size()));
            close_notify_received = result <= 0 && SSL_get_error(client->ssl.get(), result) == SSL_ERROR_ZERO_RETURN;
        }
        require(close_notify_received, "dtls server close notify");
    }
    else
    {
        require(SSL_shutdown(client->ssl.get()) >= 0 && BIO_ctrl_pending(client->write_bio) > 0, "dtls client close notify");
        require(send_dtls_client_output(*client, client_socket, server_endpoint), "dtls client close notify send");
        drain_io(io);
    }
    require(session->local_port() == 0U, "dtls close notify shutdown");

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
    media_server::test_webrtc_av1_sdp_answer();
    std::cout << "[pass] webrtc_av1_sdp_answer\n";
    media_server::test_webrtc_video_codec_parameters();
    std::cout << "[pass] webrtc_video_codec_parameters\n";
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
    media_server::test_webrtc_opus_receive_limits();
    std::cout << "[pass] webrtc_opus_receive_limits\n";
    media_server::test_webrtc_opus_source_negotiation();
    std::cout << "[pass] webrtc_opus_source_negotiation\n";
    media_server::test_webrtc_g711_source_negotiation();
    std::cout << "[pass] webrtc_g711_source_negotiation\n";
    media_server::test_webrtc_transport_contract();
    std::cout << "[pass] webrtc_transport_contract\n";
    media_server::test_whep_session_startup_errors();
    std::cout << "[pass] whep_session_startup_errors\n";
    media_server::test_whep_session_lifecycle();
    std::cout << "[pass] whep_session_lifecycle\n";
    media_server::test_whep_opus_source_session_lifecycle();
    std::cout << "[pass] whep_opus_source_session_lifecycle\n";
    media_server::test_whep_negotiated_track_lifecycle();
    std::cout << "[pass] whep_negotiated_track_lifecycle\n";
    media_server::test_whep_self_owned_lifecycle();
    std::cout << "[pass] whep_self_owned_lifecycle\n";
    media_server::test_http_head_response_contract();
    std::cout << "[pass] http_head_response_contract\n";
    media_server::test_http_method_contract();
    std::cout << "[pass] http_method_contract\n";
    media_server::test_whep_http_cors();
    std::cout << "[pass] whep_http_cors\n";
    media_server::test_whep_multi_session_isolation();
    std::cout << "[pass] whep_multi_session_isolation\n";
    media_server::test_whep_establishment_timeout();
    std::cout << "[pass] whep_establishment_timeout\n";
    media_server::test_whep_ice_activity_timeout();
    std::cout << "[pass] whep_ice_activity_timeout\n";
    media_server::test_stun_ice_connectivity_check_contract();
    std::cout << "[pass] stun_ice_connectivity_check_contract\n";
    media_server::test_whep_stun_unknown_attribute_contract();
    std::cout << "[pass] whep_stun_unknown_attribute_contract\n";
    media_server::test_whep_ice_lite();
    std::cout << "[pass] whep_ice_lite\n";
    media_server::test_whep_selected_bundle_transport();
    std::cout << "[pass] whep_selected_bundle_transport\n";
    media_server::test_whep_dtls(media_server::codec_id::h264, "SRTP_AEAD_AES_128_GCM", true);
    std::cout << "[pass] whep_dtls_h264_gcm128\n";
    media_server::test_whep_dtls(media_server::codec_id::h265, "SRTP_AEAD_AES_256_GCM", false);
    std::cout << "[pass] whep_dtls_h265_gcm256\n";
    media_server::test_whep_dtls(media_server::codec_id::h264, "SRTP_AES128_CM_SHA1_80", false);
    std::cout << "[pass] whep_dtls_h264_sha1_80\n";
    std::cout << "all tests passed\n";
    return 0;
}
