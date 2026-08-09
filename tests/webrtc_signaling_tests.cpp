#include "media/core/media_stream.h"
#include "media/core/stream_registry.h"
#include "media/webrtc/dtls_certificate.h"
#include "media/webrtc/srtp_transport.h"
#include "media/webrtc/webrtc_sdp.h"
#include "media/webrtc/whep_service.h"
#include "media/webrtc/whep_session.h"

#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/udp.hpp>
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
#include <utility>
#include <vector>

namespace media_server
{
namespace
{

constexpr track_id video_track_id = 1;
constexpr track_id audio_track_id = 2;

const std::vector<std::uint8_t> h264_config{
    0x00, 0x00, 0x00, 0x01,
    0x67, 0x42, 0xc0, 0x1f, 0xda, 0x01, 0xe0, 0x08, 0x9f, 0x97, 0x01, 0x6e, 0x40,
    0x00, 0x00, 0x00, 0x01,
    0x68, 0xce, 0x3c, 0x80,
};

const std::vector<std::uint8_t> aac_asc{0x12, 0x10};

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
    "a=fmtp:111 minptime=10;useinbandfec=1\r\n"
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
    void operator()(SSL_CTX* value) const noexcept
    {
        SSL_CTX_free(value);
    }
};

struct ssl_deleter
{
    void operator()(SSL* value) const noexcept
    {
        SSL_free(value);
    }
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
    if (!client.context ||
        SSL_CTX_set_min_proto_version(client.context.get(), DTLS1_2_VERSION) != 1 ||
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

bool send_dtls_client_output(
    dtls_test_client& client,
    boost::asio::ip::udp::socket& socket,
    const boost::asio::ip::udp::endpoint& server_endpoint)
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
            const auto payload_size =
                (static_cast<std::size_t>(output[offset + 11U]) << 8U) |
                static_cast<std::size_t>(output[offset + 12U]);
            const auto record_size = record_header_size + payload_size;
            if (record_size > output.size() - offset)
            {
                return false;
            }
            static_cast<void>(socket.send_to(
                boost::asio::buffer(output.data() + offset, record_size),
                server_endpoint));
            offset += record_size;
        }
    }
    return true;
}

bool drive_dtls_client(
    boost::asio::io_context& io,
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

std::vector<std::uint8_t> export_dtls_srtp_material(SSL* ssl)
{
    const auto* profile = SSL_get_selected_srtp_profile(ssl);
    if (profile == nullptr || profile->name == nullptr)
    {
        return {};
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
        return {};
    }

    std::vector<std::uint8_t> output(2U * (key_size + salt_size));
    constexpr std::string_view label = "EXTRACTOR-dtls_srtp";
    if (SSL_export_keying_material(
            ssl,
            output.data(),
            output.size(),
            label.data(),
            label.size(),
            nullptr,
            0,
            0) != 1)
    {
        return {};
    }
    return output;
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
    const auto* result = HMAC(
        EVP_sha1(),
        password.data(),
        static_cast<int>(password.size()),
        data.data(),
        data.size(),
        digest.data(),
        &size);
    require(result != nullptr && size == digest.size(), "stun hmac");
    return digest;
}

std::uint32_t stun_fingerprint(std::span<const std::uint8_t> data)
{
    boost::crc_32_type crc;
    crc.process_bytes(data.data(), data.size());
    return crc.checksum() ^ 0x5354554eU;
}

std::vector<std::uint8_t> make_stun_request(
    std::string_view username,
    std::string_view password,
    const std::array<std::uint8_t, 12>& transaction_id,
    bool use_candidate)
{
    std::vector<std::uint8_t> packet;
    append_u16(packet, 0x0001);
    append_u16(packet, 0);
    append_u32(packet, stun_magic_cookie);
    packet.insert(packet.end(), transaction_id.begin(), transaction_id.end());

    append_stun_attribute(
        packet,
        0x0006,
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(username.data()), username.size()));
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

std::vector<std::uint8_t> exchange_stun(
    boost::asio::io_context& io,
    boost::asio::ip::udp::socket& client,
    const boost::asio::ip::udp::endpoint& server_endpoint,
    const std::vector<std::uint8_t>& request)
{
    std::array<std::uint8_t, 2048> buffer{};
    boost::asio::ip::udp::endpoint sender;
    boost::system::error_code receive_error;
    std::size_t received = 0;
    bool complete = false;

    client.async_receive_from(
        boost::asio::buffer(buffer),
        sender,
        [&](boost::system::error_code error, std::size_t size) {
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

void require_stun_success(
    std::span<const std::uint8_t> response,
    const std::array<std::uint8_t, 12>& transaction_id)
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
        .config_version = 1,
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
        .config_version = 1,
    };
}

media_frame make_video_key_frame()
{
    return media_frame{
        .track = video_track_id,
        .dts_ns = 0,
        .pts_ns = 0,
        .duration_ns = 40'000'000,
        .key_frame = true,
        .payload = std::make_shared<const std::vector<std::uint8_t>>(std::vector<std::uint8_t>{
            0x00, 0x00, 0x00, 0x01,
            0x65, 0x88, 0x84, 0x21, 0xa0, 0x10, 0x08, 0x04,
        }),
    };
}

void test_webrtc_sdp_answer()
{
    const auto offer = parse_webrtc_offer(webrtc_offer_sdp);
    require(offer.has_value(), "parse webrtc offer");

    const auto answer = make_webrtc_answer(
        *offer,
        {make_video_track(), make_audio_track()},
        webrtc_answer_config{
            .address = boost::asio::ip::make_address("127.0.0.1"),
            .port = 40000,
            .ice_ufrag = "serverufrag",
            .ice_pwd = "serverpassword1234567890",
            .fingerprint = "AA:BB:CC:DD",
        });
    require(answer.has_value(), "make webrtc answer");
    require(answer->video_payload_type == 102, "webrtc negotiated h264 payload");
    require(answer->audio_payload_type == 111, "webrtc negotiated opus payload");
    require(answer->sdp.find("a=ice-lite\r\n") != std::string::npos, "webrtc ice lite");
    require(answer->sdp.find("a=end-of-candidates\r\n") != std::string::npos, "webrtc complete candidates");
    require(answer->sdp.find("trickle") == std::string::npos, "webrtc no trickle");
    require(answer->sdp.find("m=video 40000 UDP/TLS/RTP/SAVPF 102\r\n") != std::string::npos, "webrtc h264 payload selection");
    require(answer->sdp.find("a=rtpmap:102 H264/90000\r\n") != std::string::npos, "webrtc h264 rtpmap");
    require(answer->sdp.find("profile-level-id=42c01f") != std::string::npos, "webrtc source h264 profile");
    require(answer->sdp.find("m=audio 40000 UDP/TLS/RTP/SAVPF 111\r\n") != std::string::npos, "webrtc opus payload selection");
    require(answer->sdp.find("a=rtpmap:111 opus/48000/2\r\n") != std::string::npos, "webrtc opus rtpmap");
    require(answer->sdp.find("a=sendonly\r\n") != std::string::npos, "webrtc sendonly");
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

    const auto created = whep.create("live/test", webrtc_offer_sdp);
    require(created.error == whep_create_error::none, "whep create");
    require(!created.session_id.empty(), "whep session id");
    require(created.location == "/whep/session/" + created.session_id, "whep location");
    require(created.answer_sdp.find("a=ice-lite\r\n") != std::string::npos, "whep answer sdp");
    require(created.answer_sdp.find("a=candidate:1 1 UDP 2130706431 127.0.0.1 ") != std::string::npos, "whep host candidate");
    require(whep.remove(created.session_id), "whep delete");
    require(!whep.remove(created.session_id), "whep delete once");
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

    auto session = std::make_shared<whep_session>(
        io,
        stream,
        boost::asio::ip::make_address("127.0.0.1"),
        certificate);
    require(session->start(*offer), "ice session start");

    const auto local_ufrag = sdp_attribute(session->answer_sdp(), "ice-ufrag");
    const auto local_pwd = sdp_attribute(session->answer_sdp(), "ice-pwd");
    require(!local_ufrag.empty() && !local_pwd.empty(), "ice local credentials");

    boost::asio::ip::udp::socket client(
        io,
        boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v4(), 0));
    const boost::asio::ip::udp::endpoint server_endpoint(
        boost::asio::ip::make_address("127.0.0.1"),
        session->local_port());
    const auto username = local_ufrag + ":remotevideo";

    const std::array<std::uint8_t, 12> check_id{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    const auto check_response = exchange_stun(
        io,
        client,
        server_endpoint,
        make_stun_request(username, local_pwd, check_id, false));
    require_stun_success(check_response, check_id);
    require(!session->ice_connected(), "ice check not nominated");

    const std::array<std::uint8_t, 12> nominate_id{11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0};
    const auto nominate_response = exchange_stun(
        io,
        client,
        server_endpoint,
        make_stun_request(username, local_pwd, nominate_id, true));
    require_stun_success(nominate_response, nominate_id);
    require(session->ice_connected(), "ice nominated");

    const auto remote = session->remote_endpoint();
    require(remote.has_value(), "ice remote endpoint");
    require(remote->address().is_loopback(), "ice remote address");
    require(remote->port() == client.local_endpoint().port(), "ice remote port");

    session->close();
    boost::system::error_code error;
    client.close(error);
}


void test_whep_dtls()
{
    boost::asio::io_context io;
    auto stream = std::make_shared<media_stream>("live/dtls");
    require(stream->update_track(make_video_track()), "dtls video track");
    require(stream->update_track(make_audio_track()), "dtls audio track");

    auto server_certificate = dtls_certificate::create();
    auto client_certificate = dtls_certificate::create();
    require(server_certificate != nullptr && client_certificate != nullptr, "dtls certificates");

    const auto offer_sdp = offer_with_fingerprint(client_certificate->sha256_fingerprint());
    const auto offer = parse_webrtc_offer(offer_sdp);
    require(offer.has_value(), "dtls parse offer");

    auto session = std::make_shared<whep_session>(
        io,
        stream,
        boost::asio::ip::make_address("127.0.0.1"),
        server_certificate);
    require(session->start(*offer), "dtls session start");

    const auto local_ufrag = sdp_attribute(session->answer_sdp(), "ice-ufrag");
    const auto local_pwd = sdp_attribute(session->answer_sdp(), "ice-pwd");
    require(!local_ufrag.empty() && !local_pwd.empty(), "dtls ice credentials");

    boost::asio::ip::udp::socket client_socket(
        io,
        boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v4(), 0));
    const boost::asio::ip::udp::endpoint server_endpoint(
        boost::asio::ip::make_address("127.0.0.1"),
        session->local_port());
    const std::array<std::uint8_t, 12> nominate_id{9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 1, 2};
    const auto nominate_response = exchange_stun(
        io,
        client_socket,
        server_endpoint,
        make_stun_request(local_ufrag + ":remotevideo", local_pwd, nominate_id, true));
    require_stun_success(nominate_response, nominate_id);
    require(session->ice_connected(), "dtls ice connected");

    auto client = make_dtls_test_client(client_certificate);
    require(client.has_value(), "dtls client create");
    require(drive_dtls_client(io, client_socket, server_endpoint, *session, *client), "dtls handshake");
    require(session->dtls_connected(), "dtls server connected");

    const auto& server_material = session->srtp_keying_material();
    require(server_material.has_value(), "dtls server srtp material");
    require(!server_material->profile.empty(), "dtls srtp profile");
    require(!server_material->client_write_key.empty(), "dtls client key");
    require(!server_material->server_write_key.empty(), "dtls server key");

    const auto client_material = export_dtls_srtp_material(client->ssl.get());
    require(!client_material.empty(), "dtls client srtp material");
    std::vector<std::uint8_t> server_raw;
    server_raw.insert(server_raw.end(), server_material->client_write_key.begin(), server_material->client_write_key.end());
    server_raw.insert(server_raw.end(), server_material->server_write_key.begin(), server_material->server_write_key.end());
    server_raw.insert(server_raw.end(), server_material->client_write_salt.begin(), server_material->client_write_salt.end());
    server_raw.insert(server_raw.end(), server_material->server_write_salt.begin(), server_material->server_write_salt.end());
    require(server_raw == client_material, "dtls srtp material match");
    require(session->srtp_started(), "srtp server started");

    dtls_srtp_keying_material peer_material{
        .profile = server_material->profile,
        .client_write_key = server_material->server_write_key,
        .client_write_salt = server_material->server_write_salt,
        .server_write_key = server_material->client_write_key,
        .server_write_salt = server_material->client_write_salt,
    };
    srtp_transport peer_srtp;
    require(peer_srtp.start(peer_material), "srtp peer start");
    require(stream->publish(make_video_key_frame()), "srtp publish video");

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
    require(clear_rtp->bytes.size() >= 12U, "srtp clear rtp header");
    require((clear_rtp->bytes[0] >> 6U) == 2U, "srtp clear rtp version");
    require((clear_rtp->bytes[1] & 0x7fU) == 102U, "srtp negotiated h264 payload");

    session->close();
    boost::system::error_code error;
    client_socket.close(error);
}


}    // namespace
}    // namespace media_server

int main()
{
    using namespace media_server;
    test_webrtc_sdp_answer();
    std::cout << "[pass] webrtc_sdp_answer\n";
    test_whep_session_lifecycle();
    std::cout << "[pass] whep_session_lifecycle\n";
    test_whep_ice_lite();
    std::cout << "[pass] whep_ice_lite\n";
    test_whep_dtls();
    std::cout << "[pass] whep_dtls\n";
    std::cout << "all tests passed: 4/4\n";
    return 0;
}
