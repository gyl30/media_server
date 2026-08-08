#include "media/core/media_stream.h"
#include "media/core/stream_registry.h"
#include "media/webrtc/dtls_certificate.h"
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

#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
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
    require(answer->find("a=ice-lite\r\n") != std::string::npos, "webrtc ice lite");
    require(answer->find("a=end-of-candidates\r\n") != std::string::npos, "webrtc complete candidates");
    require(answer->find("trickle") == std::string::npos, "webrtc no trickle");
    require(answer->find("m=video 40000 UDP/TLS/RTP/SAVPF 102\r\n") != std::string::npos, "webrtc h264 payload selection");
    require(answer->find("a=rtpmap:102 H264/90000\r\n") != std::string::npos, "webrtc h264 rtpmap");
    require(answer->find("profile-level-id=42c01f") != std::string::npos, "webrtc source h264 profile");
    require(answer->find("m=audio 40000 UDP/TLS/RTP/SAVPF 111\r\n") != std::string::npos, "webrtc opus payload selection");
    require(answer->find("a=rtpmap:111 opus/48000/2\r\n") != std::string::npos, "webrtc opus rtpmap");
    require(answer->find("a=sendonly\r\n") != std::string::npos, "webrtc sendonly");
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
    std::cout << "all tests passed: 3/3\n";
    return 0;
}
