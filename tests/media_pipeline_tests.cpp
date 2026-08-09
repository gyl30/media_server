#include "media/codec/codec_utils.h"
#include "media/core/media_sink.h"
#include "media/core/media_stream.h"
#include "media/hls/hls_output.h"
#include "media/webrtc/webrtc_output.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <span>
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

const std::vector<std::vector<std::uint8_t>> valid_aac_adts_frames{
    {0xff, 0xf1, 0x50, 0x80, 0x03, 0xdf, 0xfc, 0xde, 0x02, 0x00, 0x4c, 0x61, 0x76, 0x63, 0x36, 0x31,
     0x2e, 0x31, 0x39, 0x2e, 0x31, 0x30, 0x31, 0x00, 0x42, 0x20, 0x08, 0xc1, 0x18, 0x38},
    {0xff, 0xf1, 0x50, 0x80, 0x01, 0xbf, 0xfc, 0x21, 0x10, 0x04, 0x60, 0x8c, 0x1c},
    {0xff, 0xf1, 0x50, 0x80, 0x01, 0xbf, 0xfc, 0x21, 0x10, 0x04, 0x60, 0x8c, 0x1c},
    {0xff, 0xf1, 0x50, 0x80, 0x01, 0xbf, 0xfc, 0x21, 0x10, 0x04, 0x60, 0x8c, 0x1c},
};

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


media_frame make_video_frame(std::int64_t pts_ns, bool key_frame)
{
    std::vector<std::uint8_t> bytes;
    if (key_frame)
    {
        bytes = h264_config;
        const std::vector<std::uint8_t> idr{
            0x00, 0x00, 0x00, 0x01,
            0x65, 0x88, 0x84, 0x21, 0xa0,
        };
        bytes.insert(bytes.end(), idr.begin(), idr.end());
    }
    else
    {
        bytes = {
            0x00, 0x00, 0x00, 0x01,
            0x41, 0x9a, 0x22, 0x11,
        };
    }
    return media_frame{
        .track = video_track_id,
        .dts_ns = pts_ns,
        .pts_ns = pts_ns,
        .duration_ns = 40'000'000,
        .key_frame = key_frame,
        .payload = std::make_shared<const std::vector<std::uint8_t>>(std::move(bytes)),
    };
}

media_frame make_audio_frame(std::int64_t pts_ns)
{
    const std::vector<std::uint8_t> raw{0x21, 0x10, 0x56, 0xe5, 0x00, 0x11, 0x22, 0x33};
    auto adts = make_adts_frame(aac_asc, raw);
    require(!adts.empty(), "make adts");
    return media_frame{
        .track = audio_track_id,
        .dts_ns = pts_ns,
        .pts_ns = pts_ns,
        .duration_ns = 23'219'954,
        .key_frame = false,
        .payload = std::make_shared<const std::vector<std::uint8_t>>(std::move(adts)),
    };
}

class counting_sink final : public media_sink
{
public:
    void on_track(const media_track&) override { ++tracks; }
    void on_frame(const media_frame&) override { ++frames; }
    void on_end() override { ++ends; }

    std::size_t tracks{};
    std::size_t frames{};
    std::size_t ends{};
};

class self_removing_sink final : public media_sink
{
public:
    explicit self_removing_sink(media_stream& stream) : stream_(stream) {}

    void on_track(const media_track&) override {}
    void on_frame(const media_frame&) override
    {
        ++frames;
        static_cast<void>(stream_.remove_sink(this));
    }
    void on_end() override {}

    std::size_t frames{};

private:
    media_stream& stream_;
};

void test_internal_format_contract()
{
    const auto avcc = h264_annex_b_to_avcc(h264_config);
    require(!avcc.empty(), "annex-b to avcc");
    const auto annex_b = h264_avcc_to_annex_b(avcc);
    require(!annex_b.empty(), "avcc to annex-b");
    require(annex_b.size() >= 8, "annex-b config size");
    require(annex_b[0] == 0 && annex_b[1] == 0 && annex_b[2] == 0 && annex_b[3] == 1,
            "annex-b four-byte start code");

    const std::vector<std::uint8_t> raw{0x11, 0x22, 0x33, 0x44};
    const auto adts = make_adts_frame(aac_asc, raw);
    require(adts.size() > raw.size(), "adts header exists");
    const auto aac = parse_aac_adts(adts);
    require(aac.has_value(), "parse adts");
    require(aac->sample_rate == 44'100 && aac->channel_count == 2, "adts aac config");
}

void test_media_stream_fanout_and_reentrancy()
{
    media_stream stream("live/test");
    require(stream.update_track(make_video_track()), "add video track");

    auto first = std::make_shared<counting_sink>();
    auto removing = std::make_shared<self_removing_sink>(stream);
    auto second = std::make_shared<counting_sink>();

    require(stream.add_sink(first), "add first sink");
    require(stream.add_sink(removing), "add removing sink");
    require(stream.add_sink(second), "add second sink");
    require(first->tracks == 1 && second->tracks == 1, "late sink track snapshot");

    require(stream.publish(make_video_frame(0, true)), "publish first frame");
    require(removing->frames == 1, "self-removing sink receives first frame");
    require(first->frames == 1 && second->frames == 1, "other sinks survive reentrant remove");

    require(stream.publish(make_video_frame(40'000'000, false)), "publish second frame");
    require(removing->frames == 1, "removed sink not called again");
    require(first->frames == 2 && second->frames == 2, "remaining sinks continue");

    stream.end();
    require(first->ends == 1 && second->ends == 1, "stream end fanout");
}

void test_hls_output()
{
    hls_output output(1.0, 4);
    output.on_track(make_video_track());
    output.on_track(make_audio_track());

    // 开始前的音频必须等待自然视频关键帧。
    output.on_frame(make_audio_frame(0));
    output.on_frame(make_video_frame(0, true));
    output.on_frame(make_audio_frame(20'000'000));
    output.on_frame(make_video_frame(500'000'000, false));
    output.on_frame(make_audio_frame(520'000'000));
    output.on_frame(make_video_frame(1'000'000'000, true));
    output.on_frame(make_audio_frame(1'020'000'000));
    output.on_frame(make_video_frame(1'500'000'000, false));
    output.on_frame(make_video_frame(2'000'000'000, true));
    output.on_end();

    require(output.segment_count() >= 2, "hls segment count");
    const auto first = output.segment(0);
    require(first.has_value() && !first->empty(), "first hls segment");
    require(first->size() % 188U == 0, "mpeg-ts packet alignment");
    require((*first)[0] == 0x47, "mpeg-ts sync byte");

    const auto playlist = output.playlist(".");
    require(playlist.find("#EXTM3U") != std::string::npos, "hls playlist header");
    require(playlist.find("#EXT-X-ENDLIST") != std::string::npos, "hls endlist");
}


std::uint32_t rtp_timestamp(const std::vector<std::uint8_t>& packet)
{
    require(packet.size() >= 12U, "rtp timestamp packet size");
    return (static_cast<std::uint32_t>(packet[4]) << 24U) |
        (static_cast<std::uint32_t>(packet[5]) << 16U) |
        (static_cast<std::uint32_t>(packet[6]) << 8U) |
        static_cast<std::uint32_t>(packet[7]);
}

void test_webrtc_rtp_packetizer()
{
    std::vector<std::vector<std::uint8_t>> packets;
    webrtc_output output(webrtc_output_config{.h264_payload_type = 102}, [&packets](std::span<const std::uint8_t> packet) {
        packets.emplace_back(packet.begin(), packet.end());
    });
    output.on_track(make_video_track());
    output.on_frame(make_video_frame(-40'000'000, false));
    require(packets.empty(), "webrtc waits natural key frame");
    output.on_frame(make_video_frame(0, true));

    require(output.packet_count() > 0, "webrtc h264 rtp packet count");
    require(!packets.empty() && packets.front().size() >= 12, "rtp header size");
    require((packets.front()[0] >> 6U) == 2U, "rtp version 2");
    require((packets.front()[1] & 0x7fU) == 102U, "rtp negotiated payload type");

    const auto first_timestamp = rtp_timestamp(packets.front());
    const auto first_frame_packet_count = packets.size();
    output.on_frame(make_video_frame(40'000'000, false));
    require(packets.size() > first_frame_packet_count, "webrtc second h264 frame packetized");
    require(rtp_timestamp(packets.back()) - first_timestamp == 3'600U, "h264 rtp timestamp step");
}

void test_webrtc_opus_packetizer()
{
    std::vector<std::vector<std::uint8_t>> packets;
    webrtc_output output(
        webrtc_output_config{.opus_payload_type = 111},
        [&packets](std::span<const std::uint8_t> packet) {
            packets.emplace_back(packet.begin(), packet.end());
        });
    output.on_track(make_audio_track());

    std::int64_t pts_ns = 0;
    for (const auto& adts : valid_aac_adts_frames)
    {
        output.on_frame(media_frame{
            .track = audio_track_id,
            .dts_ns = pts_ns,
            .pts_ns = pts_ns,
            .duration_ns = 23'219'954,
            .key_frame = false,
            .payload = std::make_shared<const std::vector<std::uint8_t>>(adts),
        });
        pts_ns += 23'219'954;
    }

    require(output.packet_count() > 0, "webrtc opus rtp packet count");
    require(!packets.empty() && packets.front().size() >= 12, "opus rtp header size");
    require((packets.front()[0] >> 6U) == 2U, "opus rtp version 2");
    require((packets.front()[1] & 0x7fU) == 111U, "opus negotiated payload type");

    if (packets.size() >= 2U)
    {
        require(rtp_timestamp(packets[1]) - rtp_timestamp(packets[0]) == 960U, "opus rtp timestamp step");
    }
}

}    // namespace
}    // namespace media_server

int main()
{
    using namespace media_server;
    test_internal_format_contract();
    std::cout << "[pass] internal_format_contract\n";
    test_media_stream_fanout_and_reentrancy();
    std::cout << "[pass] media_stream_fanout_and_reentrancy\n";
    test_hls_output();
    std::cout << "[pass] hls_output\n";
    test_webrtc_rtp_packetizer();
    std::cout << "[pass] webrtc_rtp_packetizer\n";
    test_webrtc_opus_packetizer();
    std::cout << "[pass] webrtc_opus_packetizer\n";
    std::cout << "all tests passed: 5/5\n";
    return 0;
}
