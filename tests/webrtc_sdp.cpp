#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <boost/asio/ip/address.hpp>

#include "media/webrtc/webrtc_sdp.h"

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
    track.codec_config = {0, 0, 0, 1, 0x67,
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
        if (!answer || answer->video_codec != codec_id::h264 || answer->video_payload_type != item.payload_type ||
            answer->video_mid != "0" || answer->transport_mid != "0" ||
            answer->sdp.find(std::string("profile-level-id=") + item.expected_profile_level + "\r\n") == std::string::npos)
        {
            throw std::runtime_error(std::string(item.name) + ": missing answer or source profile/level changed");
        }
    }
    std::cout << item.name << ": PASS\n";
}
}

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
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
