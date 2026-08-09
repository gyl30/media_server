#include "media/codec/codec_utils.h"
#include "media/core/media_sink.h"
#include "media/core/media_stream.h"
#include "media/core/stream_registry.h"
#include "media/hls/hls_output.h"
#include "media/hls/hls_service.h"
#include "media/webrtc/webrtc_output.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
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
        stream_.remove_sink(*this);
    }
    void on_end() override {}

    std::size_t frames{};

private:
    media_stream& stream_;
};

class track_removing_sink final : public media_sink
{
public:
    explicit track_removing_sink(media_stream& stream) : stream_(stream) {}

    void on_track(const media_track&) override
    {
        ++tracks;
        stream_.remove_sink(*this);
    }
    void on_frame(const media_frame&) override { ++frames; }
    void on_end() override { ++ends; }

    std::size_t tracks{};
    std::size_t frames{};
    std::size_t ends{};

private:
    media_stream& stream_;
};

class track_other_removing_sink final : public media_sink
{
public:
    track_other_removing_sink(media_stream& stream, media_sink& target) : stream_(stream), target_(target) {}

    void on_track(const media_track&) override
    {
        ++tracks;
        stream_.remove_sink(target_);
    }
    void on_frame(const media_frame&) override {}
    void on_end() override {}

    std::size_t tracks{};

private:
    media_stream& stream_;
    media_sink& target_;
};

class frame_other_removing_sink final : public media_sink
{
public:
    frame_other_removing_sink(media_stream& stream, media_sink& target) : stream_(stream), target_(target) {}

    void on_track(const media_track&) override {}
    void on_frame(const media_frame&) override
    {
        ++frames;
        stream_.remove_sink(target_);
    }
    void on_end() override {}

    std::size_t frames{};

private:
    media_stream& stream_;
    media_sink& target_;
};

class track_ending_sink final : public media_sink
{
public:
    explicit track_ending_sink(media_stream& stream) : stream_(stream) {}

    void on_track(const media_track&) override
    {
        ++tracks;
        stream_.end();
    }
    void on_frame(const media_frame&) override { ++frames; }
    void on_end() override { ++ends; }

    std::size_t tracks{};
    std::size_t frames{};
    std::size_t ends{};

private:
    media_stream& stream_;
};

class frame_ending_sink final : public media_sink
{
public:
    explicit frame_ending_sink(media_stream& stream) : stream_(stream) {}

    void on_track(const media_track&) override { ++tracks; }
    void on_frame(const media_frame&) override
    {
        ++frames;
        stream_.end();
    }
    void on_end() override { ++ends; }

    std::size_t tracks{};
    std::size_t frames{};
    std::size_t ends{};

private:
    media_stream& stream_;
};

class generation_replacing_sink final : public media_sink
{
public:
    generation_replacing_sink(stream_registry& registry, std::shared_ptr<media_stream> replacement)
        : registry_(registry), replacement_(std::move(replacement))
    {
    }

    void on_track(const media_track&) override {}
    void on_frame(const media_frame&) override {}
    void on_end() override
    {
        old_generation_hidden = !registry_.find(replacement_->name());
        replacement_added = registry_.add(replacement_);
    }

    bool old_generation_hidden{};
    bool replacement_added{};

private:
    stream_registry& registry_;
    std::shared_ptr<media_stream> replacement_;
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

    media_stream track_remove_stream("live/track-remove");
    require(track_remove_stream.update_track(make_video_track()), "track remove video");
    require(track_remove_stream.update_track(make_audio_track()), "track remove audio");
    auto track_removing = std::make_shared<track_removing_sink>(track_remove_stream);
    require(!track_remove_stream.add_sink(track_removing), "track callback removes pending sink");
    require(track_removing->tracks == 1, "removed sink stops track replay");
    require(track_remove_stream.publish(make_video_frame(0, true)), "track remove publish");
    require(track_removing->frames == 0, "track removed sink receives no frame");

    media_stream track_end_stream("live/track-end");
    require(track_end_stream.update_track(make_video_track()), "track end video");
    require(track_end_stream.update_track(make_audio_track()), "track end audio");
    auto track_ending = std::make_shared<track_ending_sink>(track_end_stream);
    require(!track_end_stream.add_sink(track_ending), "track callback ends stream");
    require(track_end_stream.ended(), "track callback stream ended");
    require(track_ending->tracks == 1, "ended stream stops track replay");
    require(track_ending->ends == 1, "new sink receives end during track replay");

    media_stream update_remove_stream("live/update-remove");
    auto update_removed = std::make_shared<counting_sink>();
    auto update_removing = std::make_shared<track_other_removing_sink>(update_remove_stream, *update_removed);
    require(update_remove_stream.add_sink(update_removing), "add update removing sink");
    require(update_remove_stream.add_sink(update_removed), "add update removed sink");
    require(update_remove_stream.update_track(make_video_track()), "update removes later sink");
    require(update_removing->tracks == 1, "update removing sink receives track");
    require(update_removed->tracks == 0, "update removed sink skips current track");

    media_stream publish_remove_stream("live/publish-remove");
    require(publish_remove_stream.update_track(make_video_track()), "publish remove video");
    auto publish_removed = std::make_shared<counting_sink>();
    auto publish_removing = std::make_shared<frame_other_removing_sink>(publish_remove_stream, *publish_removed);
    require(publish_remove_stream.add_sink(publish_removing), "add publish removing sink");
    require(publish_remove_stream.add_sink(publish_removed), "add publish removed sink");
    require(publish_remove_stream.publish(make_video_frame(0, true)), "publish removes later sink");
    require(publish_removing->frames == 1, "publish removing sink receives frame");
    require(publish_removed->frames == 0, "publish removed sink skips current frame");

    media_stream update_end_stream("live/update-end");
    auto update_ending = std::make_shared<track_ending_sink>(update_end_stream);
    auto update_after = std::make_shared<counting_sink>();
    require(update_end_stream.add_sink(update_ending), "add update ending sink");
    require(update_end_stream.add_sink(update_after), "add update trailing sink");
    require(update_end_stream.update_track(make_video_track()), "update callback ends stream");
    require(update_end_stream.ended(), "update callback stream ended");
    require(update_ending->tracks == 1 && update_ending->ends == 1, "update ending sink lifecycle");
    require(update_after->tracks == 0 && update_after->ends == 1, "update stops callbacks after end");

    media_stream publish_end_stream("live/publish-end");
    require(publish_end_stream.update_track(make_video_track()), "publish end video");
    auto frame_ending = std::make_shared<frame_ending_sink>(publish_end_stream);
    auto frame_after = std::make_shared<counting_sink>();
    require(publish_end_stream.add_sink(frame_ending), "add frame ending sink");
    require(publish_end_stream.add_sink(frame_after), "add frame trailing sink");
    require(publish_end_stream.publish(make_video_frame(0, true)), "frame callback ends stream");
    require(publish_end_stream.ended(), "frame callback stream ended");
    require(frame_ending->frames == 1 && frame_ending->ends == 1, "frame ending sink lifecycle");
    require(frame_after->frames == 0 && frame_after->ends == 1, "publish stops callbacks after end");
}


void test_stream_registry_generation_lifecycle()
{
    stream_registry registry;

    auto first = std::make_shared<media_stream>("live/generation");
    auto second = std::make_shared<media_stream>("live/generation");
    require(registry.add(first), "registry first generation add");
    require(!registry.add(second), "registry active generation duplicate reject");
    require(registry.find("live/generation").get() == first.get(), "registry first generation remains");

    auto replacing = std::make_shared<generation_replacing_sink>(registry, second);
    require(first->add_sink(replacing), "registry generation replacement observer add");
    first->end();
    require(replacing->old_generation_hidden, "registry ended generation hidden during end callback");
    require(replacing->replacement_added, "registry ended generation replace during end callback");
    require(registry.find("live/generation").get() == second.get(), "registry replacement generation visible");
    registry.remove(*first);
    require(registry.find("live/generation").get() == second.get(), "registry stale remove preserves replacement");

    auto ended = std::make_shared<media_stream>("live/ended");
    ended->end();
    require(!registry.add(ended), "registry ended generation add reject");

    registry.remove(*second);
    require(!registry.find("live/generation"), "registry replacement removed");
}

void test_hls_output()
{
    hls_output output(hls_config{.target_duration_seconds = 1.0, .window_size = 4});
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


void test_hls_service_lifecycle()
{
    stream_registry registry;
    hls_service hls(registry, hls_config{.target_duration_seconds = 1.0, .window_size = 4});

    auto first = std::make_shared<media_stream>("live/hls");
    require(registry.add(first), "hls first stream add");
    require(first->update_track(make_video_track()), "hls first track");
    require(hls.segment_count("live/hls") == 0U, "hls first output create");

    require(first->publish(make_video_frame(0, true)), "hls first key frame");
    require(first->publish(make_video_frame(1'000'000'000, true)), "hls first segment boundary");
    first->end();
    registry.remove(*first);

    const auto ended_playlist = hls.playlist("live/hls");
    require(ended_playlist.has_value(), "hls ended playlist retained");
    require(ended_playlist->find("#EXT-X-ENDLIST") != std::string::npos, "hls ended playlist marker");
    const auto ended_segment = hls.segment("live/hls", 0);
    require(ended_segment.has_value() && !ended_segment->empty(), "hls ended segment retained");

    auto second = std::make_shared<media_stream>("live/hls");
    require(registry.add(second), "hls replacement stream add");
    require(second->update_track(make_video_track()), "hls replacement track");
    require(hls.segment_count("live/hls") == 0U, "hls replacement output reset");
    require(!hls.segment("live/hls", 0).has_value(), "hls replacement drops old segment");

    stream_registry expiring_registry;
    hls_service expiring_hls(
        expiring_registry,
        hls_config{.target_duration_seconds = 0.001, .window_size = 1});
    auto expiring_stream = std::make_shared<media_stream>("live/expiring");
    require(expiring_registry.add(expiring_stream), "hls expiring stream add");
    require(expiring_stream->update_track(make_video_track()), "hls expiring track");
    require(expiring_hls.segment_count("live/expiring") == 0U, "hls expiring output create");
    expiring_stream->end();
    expiring_registry.remove(*expiring_stream);
    require(expiring_hls.playlist("live/expiring").has_value(), "hls ended output initially retained");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    require(!expiring_hls.playlist("live/expiring").has_value(), "hls ended output expires");
}


std::uint32_t rtp_timestamp(const std::vector<std::uint8_t>& packet)
{
    require(packet.size() >= 12U, "rtp timestamp packet size");
    return (static_cast<std::uint32_t>(packet[4]) << 24U) |
        (static_cast<std::uint32_t>(packet[5]) << 16U) |
        (static_cast<std::uint32_t>(packet[6]) << 8U) |
        static_cast<std::uint32_t>(packet[7]);
}

std::uint16_t network_u16(std::span<const std::uint8_t> data, std::size_t offset)
{
    require(offset + 2U <= data.size(), "network u16 range");
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(data[offset]) << 8U) |
        static_cast<std::uint16_t>(data[offset + 1U]));
}

std::uint32_t network_u32(std::span<const std::uint8_t> data, std::size_t offset)
{
    require(offset + 4U <= data.size(), "network u32 range");
    return (static_cast<std::uint32_t>(data[offset]) << 24U) |
        (static_cast<std::uint32_t>(data[offset + 1U]) << 16U) |
        (static_cast<std::uint32_t>(data[offset + 2U]) << 8U) |
        static_cast<std::uint32_t>(data[offset + 3U]);
}

std::uint32_t rtp_ssrc(const std::vector<std::uint8_t>& packet)
{
    require(packet.size() >= 12U, "rtp ssrc packet size");
    return network_u32(packet, 8U);
}

std::uint32_t require_rtcp_sender_report(
    const std::vector<std::uint8_t>& packet,
    std::string_view expected_cname)
{
    require(packet.size() >= 40U, "rtcp compound size");
    require((packet[0] >> 6U) == 2U, "rtcp sr version");
    require(packet[1] == 200U, "rtcp sender report type");

    const auto sr_size = (static_cast<std::size_t>(network_u16(packet, 2U)) + 1U) * 4U;
    require(sr_size >= 28U && sr_size + 12U <= packet.size(), "rtcp sender report size");
    const auto sender_ssrc = network_u32(packet, 4U);
    require(network_u32(packet, 20U) > 0U, "rtcp sender packet count");
    require(network_u32(packet, 24U) > 0U, "rtcp sender octet count");

    require((packet[sr_size] >> 6U) == 2U, "rtcp sdes version");
    require(packet[sr_size + 1U] == 202U, "rtcp sdes type");
    const auto sdes_size = (static_cast<std::size_t>(network_u16(packet, sr_size + 2U)) + 1U) * 4U;
    require(sr_size + sdes_size == packet.size(), "rtcp compound boundary");
    require(network_u32(packet, sr_size + 4U) == sender_ssrc, "rtcp sdes sender ssrc");
    require(packet[sr_size + 8U] == 1U, "rtcp sdes cname type");

    const auto cname_size = static_cast<std::size_t>(packet[sr_size + 9U]);
    require(sr_size + 10U + cname_size <= packet.size(), "rtcp cname range");
    const auto cname = std::string_view(
        reinterpret_cast<const char*>(packet.data() + sr_size + 10U),
        cname_size);
    require(cname == expected_cname, "rtcp sdes cname");
    return sender_ssrc;
}

void test_webrtc_rtp_packetizer()
{
    std::vector<std::vector<std::uint8_t>> packets;
    webrtc_output output(webrtc_output_config{.h264_payload_type = 102, .rtcp_cname = {}}, [&packets](std::span<const std::uint8_t> packet) {
        packets.emplace_back(packet.begin(), packet.end());
    });
    output.on_track(make_video_track());
    output.on_frame(make_video_frame(-40'000'000, false));
    require(packets.empty(), "webrtc waits natural key frame");
    output.on_frame(make_video_frame(0, true));

    require(!packets.empty() && packets.front().size() >= 12, "rtp header size");
    require((packets.front()[0] >> 6U) == 2U, "rtp version 2");
    require((packets.front()[1] & 0x7fU) == 102U, "rtp negotiated payload type");

    const auto first_timestamp = rtp_timestamp(packets.front());
    const auto first_frame_packet_count = packets.size();
    output.on_frame(make_video_frame(40'000'000, false));
    require(packets.size() > first_frame_packet_count, "webrtc second h264 frame packetized");
    require(rtp_timestamp(packets.back()) - first_timestamp == 3'600U, "h264 rtp timestamp step");
}

void test_webrtc_opus_channel_count(int channel_count)
{
    std::vector<std::vector<std::uint8_t>> packets;
    webrtc_output output(
        webrtc_output_config{
            .opus_payload_type = 111,
            .opus_channel_count = channel_count,
            .rtcp_cname = {},
        },
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

    require(!packets.empty() && packets.front().size() >= 12, "opus rtp header size");
    require((packets.front()[0] >> 6U) == 2U, "opus rtp version 2");
    require((packets.front()[1] & 0x7fU) == 111U, "opus negotiated payload type");

    if (packets.size() >= 2U)
    {
        require(rtp_timestamp(packets[1]) - rtp_timestamp(packets[0]) == 960U, "opus rtp timestamp step");
    }
}

void test_webrtc_rtcp_sender()
{
    constexpr std::string_view cname = "webrtc-test-cname";
    std::vector<std::vector<std::uint8_t>> rtp_packets;
    std::vector<std::vector<std::uint8_t>> rtcp_packets;
    webrtc_output output(
        webrtc_output_config{
            .h264_payload_type = 102,
            .opus_payload_type = 111,
            .opus_channel_count = 2,
            .rtcp_cname = std::string(cname),
        },
        [&rtp_packets](std::span<const std::uint8_t> packet) {
            rtp_packets.emplace_back(packet.begin(), packet.end());
        },
        [&rtcp_packets](std::span<const std::uint8_t> packet) {
            rtcp_packets.emplace_back(packet.begin(), packet.end());
        });

    output.on_track(make_video_track());
    output.on_track(make_audio_track());
    output.on_frame(make_video_frame(0, true));

    std::int64_t audio_pts_ns = 0;
    for (const auto& adts : valid_aac_adts_frames)
    {
        output.on_frame(media_frame{
            .track = audio_track_id,
            .dts_ns = audio_pts_ns,
            .pts_ns = audio_pts_ns,
            .duration_ns = 23'219'954,
            .key_frame = false,
            .payload = std::make_shared<const std::vector<std::uint8_t>>(adts),
        });
        audio_pts_ns += 23'219'954;
    }

    std::optional<std::uint32_t> video_ssrc;
    std::optional<std::uint32_t> audio_ssrc;
    for (const auto& packet : rtp_packets)
    {
        require(packet.size() >= 12U, "rtcp sender rtp header");
        const auto payload_type = static_cast<std::uint8_t>(packet[1] & 0x7fU);
        if (payload_type == 102U)
        {
            video_ssrc = rtp_ssrc(packet);
        }
        else if (payload_type == 111U)
        {
            audio_ssrc = rtp_ssrc(packet);
        }
    }
    require(video_ssrc.has_value(), "rtcp sender video ssrc");
    require(audio_ssrc.has_value(), "rtcp sender audio ssrc");
    require(*video_ssrc != *audio_ssrc, "rtcp sender independent ssrc");

    bool video_report = false;
    bool audio_report = false;
    for (const auto& packet : rtcp_packets)
    {
        const auto sender_ssrc = require_rtcp_sender_report(packet, cname);
        video_report = video_report || sender_ssrc == *video_ssrc;
        audio_report = audio_report || sender_ssrc == *audio_ssrc;
    }
    require(video_report, "rtcp video sender report");
    require(audio_report, "rtcp audio sender report");
}

void test_webrtc_opus_packetizer()
{
    test_webrtc_opus_channel_count(1);
    test_webrtc_opus_channel_count(2);
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
    test_stream_registry_generation_lifecycle();
    std::cout << "[pass] stream_registry_generation_lifecycle\n";
    test_hls_output();
    std::cout << "[pass] hls_output\n";
    test_hls_service_lifecycle();
    std::cout << "[pass] hls_service_lifecycle\n";
    test_webrtc_rtp_packetizer();
    std::cout << "[pass] webrtc_rtp_packetizer\n";
    test_webrtc_opus_packetizer();
    std::cout << "[pass] webrtc_opus_packetizer\n";
    test_webrtc_rtcp_sender();
    std::cout << "[pass] webrtc_rtcp_sender\n";
    std::cout << "all tests passed: 7/7\n";
    return 0;
}
