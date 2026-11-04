#include <array>
#include <chrono>
#include <atomic>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <ctime>
#include <cstdint>
#include <cstdlib>

#include <srtp.h>
#include <spdlog/spdlog.h>

#include "media/core/media_types.h"
#include "media/webrtc/srtp_transport.h"
#include "media/webrtc/dtls_transport.h"
#include "media/webrtc/webrtc_packetizer.h"

extern "C"
{
#include "rtmp-handshake.h"
}

namespace
{

using media_server::codec_id;
using media_server::dtls_role;
using media_server::media_kind;
using media_server::media_frame;
using media_server::media_track;
using media_server::srtp_transport;
using media_server::webrtc_packetizer;
using media_server::webrtc_packetizer_config;
using media_server::dtls_srtp_keying_material;

int failures = 0;

void expect(bool condition, const std::string& message)
{
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", message.c_str());
    failures += condition ? 0 : 1;
}

struct header
{
    std::uint8_t payload_type{};
    std::uint16_t sequence{};
    std::uint32_t ssrc{};
};

header parse(std::span<const std::uint8_t> packet)
{
    return header{
        .payload_type = static_cast<std::uint8_t>(packet[1] & 0x7FU),
        .sequence = static_cast<std::uint16_t>((packet[2] << 8U) | packet[3]),
        .ssrc = (static_cast<std::uint32_t>(packet[8]) << 24U) | (static_cast<std::uint32_t>(packet[9]) << 16U) |
                (static_cast<std::uint32_t>(packet[10]) << 8U) | packet[11],
    };
}

const std::vector<media_track>& tracks()
{
    static const std::vector<media_track> value{
        media_track{
            .id = 1,
            .kind = media_kind::video,
            .codec = codec_id::h264,
            .clock_rate = 90'000,
            .codec_config = {0,    0,    0,    1,    0x67, 0x42, 0xc0, 0x0b, 0xdc, 0x28, 0x47, 0xe5, 0xc0, 0x44, 0x00, 0x00, 0x03, 0x00,
                             0x04, 0x00, 0x00, 0x03, 0x00, 0xc8, 0x3c, 0x50, 0xae, 0,    0,    0,    1,    0x68, 0xce, 0x0f, 0x2c, 0x80},
        },
        media_track{.id = 2, .kind = media_kind::audio, .codec = codec_id::opus, .clock_rate = 48'000, .channel_count = 2, .codec_config = {}},
    };
    return value;
}

const webrtc_packetizer_config config{
    .video_codec = codec_id::h264,
    .audio_codec = codec_id::opus,
    .video_payload_type = 102,
    .audio_payload_type = 111,
    .video_mid = "0",
    .audio_mid = "1",
    .video_mid_extension_id = 4,
    .audio_mid_extension_id = 4,
    .rtcp_cname = "check",
};

media_frame video_frame()
{
    auto payload = std::make_shared<std::vector<std::uint8_t>>(std::vector<std::uint8_t>{0, 0, 0, 1, 0x65});
    payload->resize(payload->size() + 3'000, 0x5A);
    return media_frame{.track = 1, .dts_ns = 40'000'000, .pts_ns = 40'000'000, .key_frame = true, .payload = std::move(payload)};
}

media_frame audio_frame()
{
    auto payload = std::make_shared<std::vector<std::uint8_t>>(120, 0x33);
    return media_frame{.track = 2, .dts_ns = 40'000'000, .pts_ns = 40'000'000, .payload = std::move(payload)};
}

dtls_srtp_keying_material keying_material()
{
    return dtls_srtp_keying_material{
        .profile = srtp_profile_aes128_cm_sha1_80,
        .client_write_key = std::vector<std::uint8_t>(16, 0x01),
        .client_write_salt = std::vector<std::uint8_t>(14, 0x02),
        .server_write_key = std::vector<std::uint8_t>(16, 0x03),
        .server_write_salt = std::vector<std::uint8_t>(14, 0x04),
    };
}

struct session_result
{
    header video;
    header audio;
    bool protect_failed{};
};

session_result run_session(bool with_srtp)
{
    session_result result;
    std::unique_ptr<srtp_transport> srtp;
    if (with_srtp)
    {
        srtp = std::make_unique<srtp_transport>();
        if (!srtp->startup(keying_material(), dtls_role::server))
        {
            result.protect_failed = true;
            return result;
        }
    }
    bool first_video = true;
    bool first_audio = true;
    webrtc_packetizer packetizer(
        [&](std::span<const std::uint8_t> packet)
        {
            const auto parsed = parse(packet);
            if (parsed.payload_type == config.video_payload_type && first_video)
            {
                result.video = parsed;
                first_video = false;
            }
            if (parsed.payload_type == config.audio_payload_type && first_audio)
            {
                result.audio = parsed;
                first_audio = false;
            }
            if (srtp && !srtp->protect_rtp(packet))
            {
                result.protect_failed = true;
                return -1;
            }
            return 0;
        },
        [](std::span<const std::uint8_t>) { return 0; });
    if (!packetizer.startup(tracks(), config))
    {
        result.protect_failed = true;
        return result;
    }
    const auto video = video_frame();
    const auto audio = audio_frame();
    static_cast<void>(packetizer.on_frame(video));
    static_cast<void>(packetizer.on_frame(audio));
    return result;
}

void rtmp_server_handshake(std::uint32_t timestamp)
{
    std::array<std::uint8_t, RTMP_HANDSHAKE_SIZE> c1{};
    std::array<std::uint8_t, RTMP_HANDSHAKE_SIZE> s1{};
    rtmp_handshake_s1(s1.data(), timestamp, c1.data(), c1.size());
}

void same_second_handshakes_repeat_rtp_identity()
{
    rtmp_server_handshake(1'000);
    const auto first = run_session(false);
    rtmp_server_handshake(1'000);
    const auto second = run_session(false);
    std::printf("     session 1 video ssrc %08x seq %u audio ssrc %08x seq %u\n",
                first.video.ssrc,
                first.video.sequence,
                first.audio.ssrc,
                first.audio.sequence);
    std::printf("     session 2 video ssrc %08x seq %u audio ssrc %08x seq %u\n",
                second.video.ssrc,
                second.video.sequence,
                second.audio.ssrc,
                second.audio.sequence);
    expect(first.video.ssrc != second.video.ssrc || first.video.sequence != second.video.sequence,
           "two sessions after RTMP handshakes in the same second get different video SSRC/sequence");
    expect(first.audio.ssrc != second.audio.ssrc || first.audio.sequence != second.audio.sequence,
           "two sessions after RTMP handshakes in the same second get different audio SSRC/sequence");
}

void concurrent_handshakes_do_not_collide_tracks(int sessions, int gap_us)
{
    std::atomic_bool stop{};
    std::thread handshakes(
        [&stop, gap_us]()
        {
            while (!stop.load(std::memory_order_relaxed))
            {
                rtmp_server_handshake(static_cast<std::uint32_t>(std::time(nullptr)));
                const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(gap_us);
                while (std::chrono::steady_clock::now() < until)
                {
                }
            }
        });
    int shared_ssrc = 0;
    int protect_failed = 0;
    for (int session = 0; session < sessions; ++session)
    {
        const auto result = run_session(true);
        shared_ssrc += result.video.ssrc == result.audio.ssrc ? 1 : 0;
        protect_failed += result.protect_failed ? 1 : 0;
    }
    stop.store(true, std::memory_order_relaxed);
    handshakes.join();
    std::printf("     %d sessions, handshake every %d us: video and audio share SSRC in %d, srtp_protect failed in %d\n",
                sessions,
                gap_us,
                shared_ssrc,
                protect_failed);
    expect(shared_ssrc == 0, "no session gives its video and audio tracks the same SSRC while RTMP handshakes run");
    expect(protect_failed == 0, "no session fails srtp_protect while RTMP handshakes run");
}

}    // namespace

int main(int argc, char** argv)
{
    spdlog::set_level(spdlog::level::off);
    same_second_handshakes_repeat_rtp_identity();
    concurrent_handshakes_do_not_collide_tracks(argc > 1 ? std::atoi(argv[1]) : 20'000, argc > 2 ? std::atoi(argv[2]) : 20);
    std::printf("%s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
