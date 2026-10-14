#include <array>
#include <string>
#include <vector>
#include <cstdint>
#include <iostream>
#include <stdexcept>

#include <boost/asio/ip/address.hpp>

#include "media/webrtc/webrtc_sdp.h"
#include "media/webrtc/stun_message.h"

namespace
{
using namespace media_server;

struct negotiation_case
{
    const char* name;
    std::uint32_t source_profile_level;
    const char* format_parameters;
    const char* expected_profile_level;
    int payload_type{102};
    std::uint32_t clock_rate{90'000};
    const char* encoding_name{"H264"};
};

void check(const negotiation_case& item)
{
    webrtc_codec_offer codec;
    codec.payload_type = item.payload_type;
    codec.encoding_name = item.encoding_name;
    codec.clock_rate = item.clock_rate;
    codec.format_parameters = item.format_parameters;

    webrtc_media_offer media;
    media.type = "video";
    media.port = 9;
    media.protocol = "UDP/TLS/RTP/SAVPF";
    media.mid = "0";
    media.direction = "recvonly";
    media.setup = "actpass";
    media.rtcp_mux = true;
    media.mid_extension_id = 1;
    media.formats = {std::to_string(item.payload_type)};
    media.payload_types = {item.payload_type};
    media.codecs = {codec};

    webrtc_offer offer;
    offer.bundle_mids = {"0"};
    offer.media = {media};

    media_track track;
    track.id = 1;
    track.kind = media_kind::video;
    track.codec = codec_id::h264;
    track.clock_rate = 90'000;
    // SDP selection reads the SPS profile/constraint/level bytes, not encoded media.
    track.codec_config = {0,
                          0,
                          0,
                          1,
                          0x67,
                          static_cast<std::uint8_t>(item.source_profile_level >> 16U),
                          static_cast<std::uint8_t>(item.source_profile_level >> 8U),
                          static_cast<std::uint8_t>(item.source_profile_level)};

    webrtc_answer_config config;
    config.address = boost::asio::ip::make_address("127.0.0.1");
    config.port = 49000;
    config.stream_id = "sdp-test";
    config.ice_ufrag = "test-ufrag";
    config.ice_pwd = "test-password-not-a-real-credential";
    config.fingerprint = "test-fingerprint-not-used-for-transport";

    const auto answer = make_webrtc_answer(offer, {track}, config);
    if (item.expected_profile_level == nullptr)
    {
        if (answer)
        {
            throw std::runtime_error(std::string(item.name) + ": unexpected answer");
        }
    }
    else
    {
        if (!answer || answer->video_codec != codec_id::h264 || answer->video_payload_type != item.payload_type || answer->video_mid != "0" ||
            answer->transport_mid != "0" ||
            answer->sdp.find(std::string("profile-level-id=") + item.expected_profile_level + "\r\n") == std::string::npos)
        {
            throw std::runtime_error(std::string(item.name) + ": missing answer or source profile/level changed");
        }
    }
    std::cout << item.name << ": PASS\n";
}

struct opus_case
{
    const char* name;
    std::uint16_t source_channels;
    const char* format_parameters;
    bool accepted;
};

void check_opus(const opus_case& item)
{
    webrtc_media_offer media;
    media.type = "audio";
    media.port = 9;
    media.protocol = "UDP/TLS/RTP/SAVPF";
    media.mid = "0";
    media.direction = "recvonly";
    media.setup = "actpass";
    media.rtcp_mux = true;
    media.mid_extension_id = 4;
    media.formats = {"111"};
    media.payload_types = {111};
    media.codecs = {webrtc_codec_offer{
        .payload_type = 111, .encoding_name = "opus", .clock_rate = 48'000, .channel_count = 2, .format_parameters = item.format_parameters}};

    webrtc_offer offer;
    offer.bundle_mids = {"0"};
    offer.media = {media};

    const media_track track{
        .id = 2, .kind = media_kind::audio, .codec = codec_id::opus, .clock_rate = 48'000, .channel_count = item.source_channels, .codec_config = {}};
    webrtc_answer_config config;
    config.address = boost::asio::ip::make_address("127.0.0.1");
    config.port = 49000;
    config.stream_id = "sdp-test";
    config.ice_ufrag = "test-ufrag";
    config.ice_pwd = "test-password-not-a-real-credential";
    config.fingerprint = "test-fingerprint-not-used-for-transport";

    const auto answer = make_webrtc_answer(offer, {track}, config);
    const bool accepted = answer && answer->audio_codec == codec_id::opus && answer->audio_payload_type == 111;
    if (accepted != item.accepted)
    {
        throw std::runtime_error(std::string(item.name) + ": unexpected opus passthrough negotiation");
    }
    std::cout << item.name << ": PASS\n";
}
// 平台作为 WHEP 客户端：自己的 offer 能被平台 WHEP 输出接受，应答解析出 ICE/DTLS 参数、candidate 与负载类型。
void check_whep_pull()
{
    webrtc_answer_config client;
    client.address = boost::asio::ip::make_address("127.0.0.1");
    client.port = 49002;
    client.stream_id = "pull";
    client.ice_ufrag = "pullufrag";
    client.ice_pwd = "pull-password-not-a-real-credential";
    client.fingerprint = "AA:BB";
    const auto offer = parse_webrtc_offer(make_whep_pull_offer(client));
    if (!offer || find_webrtc_transport(*offer) == nullptr || offer->media.size() != 2 || offer->media[0].direction != "recvonly" ||
        offer->media[0].candidates.size() != 1 || offer->media[0].candidates[0].port() != 49002)
    {
        throw std::runtime_error("whep pull offer not acceptable");
    }

    const media_track video{.id = 1,
                            .kind = media_kind::video,
                            .codec = codec_id::h264,
                            .clock_rate = 90'000,
                            .channel_count = 0,
                            .codec_config = {0, 0, 0, 1, 0x67, 0x64, 0x00, 0x1f}};
    const media_track audio{
        .id = 2, .kind = media_kind::audio, .codec = codec_id::opus, .clock_rate = 48'000, .channel_count = 2, .codec_config = {}};
    webrtc_answer_config server = client;
    server.port = 49004;
    server.ice_ufrag = "srvufrag";
    const auto answer = make_webrtc_answer(*offer, {video, audio}, server);
    if (!answer)
    {
        throw std::runtime_error("platform WHEP rejected pull offer");
    }
    const auto parsed = parse_whep_pull_answer(answer->sdp);
    if (!parsed || parsed->ice_ufrag != "srvufrag" || parsed->setup != "passive" || parsed->candidates.size() != 1 ||
        parsed->candidates[0] != boost::asio::ip::udp::endpoint(server.address, 49004) || parsed->video_payload_type != 99 ||
        parsed->audio_payload_type != 111)
    {
        throw std::runtime_error("whep pull answer parse mismatch");
    }

    auto without_candidate = answer->sdp;
    without_candidate.erase(without_candidate.find("a=candidate:"),
                            without_candidate.find("a=end-of-candidates") - without_candidate.find("a=candidate:"));
    auto vp8_only = answer->sdp;
    vp8_only.replace(vp8_only.find("H264/90000"), 10, "VP8/90000");
    for (const auto& invalid : {without_candidate, vp8_only, std::string("v=0\r\n")})
    {
        if (parse_whep_pull_answer(invalid))
        {
            throw std::runtime_error("invalid whep pull answer accepted");
        }
    }
    std::cout << "WHEP pull offer/answer: PASS\n";
}

// controlling 方检查请求能被对端（平台 WHEP 输出使用的解析）认证，其成功响应能被本端按对端密码校验。
void check_stun_controlling_round_trip()
{
    const std::array<std::uint8_t, 12> transaction{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    const auto request = make_stun_binding_request(transaction, "srvufrag:cliufrag", "server-password", 2'130'706'431U, 0x0102030405060708ULL);
    const auto parsed = parse_stun_binding_request(request, "srvufrag:cliufrag", "server-password");
    if (!parsed || !parsed->use_candidate || !parsed->ice_controlling || parsed->ice_controlled || !parsed->priority ||
        parsed->transaction_id != transaction || parse_stun_binding_request(request, "srvufrag:cliufrag", "wrong-password"))
    {
        throw std::runtime_error("controlling STUN request not accepted");
    }
    const auto response = make_stun_binding_success_response(
        *parsed, boost::asio::ip::udp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 5000), "server-password");
    if (parse_stun_binding_success_response(response, "server-password") != transaction ||
        parse_stun_binding_success_response(response, "wrong-password") || parse_stun_binding_success_response(request, "server-password"))
    {
        throw std::runtime_error("STUN success response verification failed");
    }
    std::cout << "STUN controlling check round trip: PASS\n";
}
}    // namespace

int main()
{
    try
    {
        const std::vector<negotiation_case> cases{
            {"baseline", 0x42001f, "packetization-mode=1;profile-level-id=42001f", "42001f"},
            {"constrained_baseline", 0x42e01f, "packetization-mode=1;profile-level-id=42e01f", "42e01f"},
            {"main", 0x4d001f, "packetization-mode=1;profile-level-id=4d001f", "4d001f"},
            {"high", 0x64001f, "packetization-mode=1;profile-level-id=64001f", "64001f"},
            {"constrained_high", 0x640c1f, "packetization-mode=1;profile-level-id=640c1f", "640c1f"},
            {"real_cb_asymmetric", 0x42e033, "packetization-mode=1;profile-level-id=42e01f;level-asymmetry-allowed=1", "42e033"},
            {"real_main_asymmetric", 0x4d6033, "packetization-mode=1;profile-level-id=4d001f;level-asymmetry-allowed=1", "4d6033"},
            {"real_main_equal_level", 0x4d6033, "packetization-mode=1;profile-level-id=4d0033", "4d6033"},
            {"main_constraint_set2", 0x4d2033, "packetization-mode=1;profile-level-id=4d0033", "4d2033"},
            {"main_reserved_bit", 0x4d6133, "packetization-mode=1;profile-level-id=4d0033", nullptr},
            {"main_constrained_baseline_subset", 0x4de033, "packetization-mode=1;profile-level-id=42e033", "4de033"},
            {"higher_receiver_level", 0x42e01f, "packetization-mode=1;profile-level-id=42e033", "42e01f"},
            {"lower_level_asymmetry_zero", 0x42e033, "packetization-mode=1;profile-level-id=42e01f;level-asymmetry-allowed=0", nullptr},
            {"lower_level_asymmetry_absent", 0x42e033, "packetization-mode=1;profile-level-id=42e01f", nullptr},
            {"lower_level_asymmetry_invalid", 0x42e033, "packetization-mode=1;profile-level-id=42e01f;level-asymmetry-allowed=2", nullptr},
            {"incompatible_profile", 0x42e033, "packetization-mode=1;profile-level-id=4d001f;level-asymmetry-allowed=1", nullptr},
            {"main_high_mismatch", 0x4d6033, "packetization-mode=1;profile-level-id=640033;level-asymmetry-allowed=1", nullptr},
            {"packetization_mode_zero", 0x42e033, "packetization-mode=0;profile-level-id=42e01f;level-asymmetry-allowed=1", nullptr},
            {"packetization_mode_absent", 0x42e033, "profile-level-id=42e01f;level-asymmetry-allowed=1", nullptr},
            {"invalid_profile_hex", 0x42e01f, "packetization-mode=1;profile-level-id=42e0zz", nullptr},
            {"invalid_profile_length", 0x42e01f, "packetization-mode=1;profile-level-id=42e01", nullptr},
            {"invalid_level", 0x42e01f, "packetization-mode=1;profile-level-id=42e000;level-asymmetry-allowed=1", nullptr},
            {"invalid_source_level", 0x42e000, "packetization-mode=1;profile-level-id=42e01f;level-asymmetry-allowed=1", nullptr},
            {"default_profile_level", 0x42000a, "packetization-mode=1", "42000a"},
            {"default_profile_level_too_low", 0x42001f, "packetization-mode=1", nullptr},
            {"wrong_clock_rate", 0x42e01f, "packetization-mode=1;profile-level-id=42e01f", nullptr, 102, 48'000},
            {"rtcp_mux_payload_conflict", 0x42e01f, "packetization-mode=1;profile-level-id=42e01f", nullptr, 80},
            {"non_h264_encoding", 0x42e01f, "packetization-mode=1;profile-level-id=42e01f", nullptr, 102, 90'000, "VP8"},
        };
        for (const auto& item : cases)
        {
            check(item);
        }
        std::cout << "H264 SDP selection: " << cases.size() << " cases PASS\n";
        check_whep_pull();
        check_stun_controlling_round_trip();

        // Chrome 默认 Opus fmtp 不带 maxaveragebitrate。
        const std::vector<opus_case> opus_cases{
            {"opus_mono_browser_default", 1, "minptime=10;useinbandfec=1", true},
            {"opus_stereo_browser_stereo", 2, "minptime=10;useinbandfec=1;stereo=1", true},
            {"opus_explicit_max_bitrate", 1, "minptime=10;useinbandfec=1;maxaveragebitrate=510000", true},
            {"opus_limited_bitrate", 1, "minptime=10;useinbandfec=1;maxaveragebitrate=64000", false},
            {"opus_stereo_without_stereo", 2, "minptime=10;useinbandfec=1", false},
        };
        for (const auto& item : opus_cases)
        {
            check_opus(item);
        }
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
