#include "media/core/media_stream.h"
#include "media/core/stream_registry.h"
#include "media/webrtc/webrtc_sdp.h"
#include "media/webrtc/whep_service.h"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>

#include <cstdlib>
#include <iostream>
#include <memory>
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

}    // namespace
}    // namespace media_server

int main()
{
    using namespace media_server;
    test_webrtc_sdp_answer();
    std::cout << "[pass] webrtc_sdp_answer\n";
    test_whep_session_lifecycle();
    std::cout << "[pass] whep_session_lifecycle\n";
    std::cout << "all tests passed: 2/2\n";
    return 0;
}
