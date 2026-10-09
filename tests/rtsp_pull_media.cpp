#include <array>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "media/core/stream_registry.h"
#include "media/net/worker_context.h"
#include "media/rtsp/rtsp_pull_media.h"

namespace
{
using namespace media_server;

// 16x16 H.264 SPS/PPS/IDR，由 FFmpeg libx264 生成。
constexpr std::array<std::uint8_t, 45> h264_idr{ 0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0xc0, 0x0a, 0xdd, 0xec, 0x04, 0x40, 0x00, 0x00, 0x03, 0x00, 0x40, 0x00, 0x00, 0x0c, 0x83, 0xc4, 0x89, 0xe0, 0x00, 0x00, 0x00, 0x01, 0x68, 0xce, 0x0f, 0xc8, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x3a, 0x26, 0x28, 0x00, 0x09, 0x02, 0xe0};
constexpr std::uint8_t video_channel = 0;
constexpr std::uint8_t audio_channel = 2;

void require(bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

std::vector<std::uint8_t> rtp(std::uint8_t payload_type, bool marker, std::uint16_t sequence, std::uint32_t timestamp, std::span<const std::uint8_t> payload)
{
    constexpr std::uint32_t ssrc = 0x01020304;
    std::vector<std::uint8_t> packet{0x80, static_cast<std::uint8_t>((marker ? 0x80U : 0U) | payload_type),
                                     static_cast<std::uint8_t>(sequence >> 8U), static_cast<std::uint8_t>(sequence),
                                     static_cast<std::uint8_t>(timestamp >> 24U), static_cast<std::uint8_t>(timestamp >> 16U),
                                     static_cast<std::uint8_t>(timestamp >> 8U), static_cast<std::uint8_t>(timestamp),
                                     static_cast<std::uint8_t>(ssrc >> 24U), static_cast<std::uint8_t>(ssrc >> 16U),
                                     static_cast<std::uint8_t>(ssrc >> 8U), static_cast<std::uint8_t>(ssrc)};
    packet.insert(packet.end(), payload.begin(), payload.end());
    return packet;
}

// 拆出 Annex-B 中的 NAL 单元（3 或 4 字节起始码）。
std::vector<std::span<const std::uint8_t>> nal_units(std::span<const std::uint8_t> annex_b)
{
    std::vector<std::pair<std::size_t, std::size_t>> ranges;
    for (std::size_t index = 0; index + 3 <= annex_b.size(); ++index)
    {
        if (annex_b[index] == 0 && annex_b[index + 1] == 0 && annex_b[index + 2] == 1)
        {
            if (!ranges.empty())
            {
                ranges.back().second = index > 0 && annex_b[index - 1] == 0 ? index - 1 : index;
            }
            ranges.emplace_back(index + 3, annex_b.size());
            index += 2;
        }
    }
    std::vector<std::span<const std::uint8_t>> units;
    for (const auto& [begin, end] : ranges)
    {
        units.push_back(annex_b.subspan(begin, end - begin));
    }
    return units;
}
}    // namespace

int main()
{
    try
    {
        worker_context worker;
        std::size_t notifications{};
        const std::string name = "rtsp/pull-unready";
        rtsp_pull_media media(worker, name, [&notifications]() { ++notifications; });
        std::vector<rtsp_pull_track_description> descriptions;
        // SDP 未携带参数集：视频轨道要等带内 SPS/PPS 才能就绪。
        descriptions.push_back(rtsp_pull_track_description{
            .kind = media_kind::video, .clock_rate = 90'000, .payload_type = 96, .encoding = "H264", .fmtp = "packetization-mode=1", .initial_track = {}});
        descriptions.push_back(rtsp_pull_track_description{
            .kind = media_kind::audio,
            .clock_rate = 8'000,
            .payload_type = 8,
            .encoding = "PCMA",
            .fmtp = {},
            .initial_track = media_track{.id = 2, .kind = media_kind::audio, .codec = codec_id::g711a, .clock_rate = 8'000, .channel_count = 1, .codec_config = {}}});
        require(media.startup(std::move(descriptions)), "pull media startup failed");

        std::uint16_t audio_sequence = 1;
        std::uint32_t audio_timestamp = 0;
        const std::vector<std::uint8_t> pcma(160, 0xd5);
        auto send_audio = [&](int count)
        {
            for (int index = 0; index < count; ++index)
            {
                const auto packet = rtp(8, true, audio_sequence++, audio_timestamp, pcma);
                audio_timestamp += 160;
                require(media.input_packet(audio_channel, packet), "audio rtp rejected");
            }
        };

        send_audio(100);
        require(stream_registry::instance().find(name) == nullptr, "stream registered without video config");
        require(notifications == 0, "unready audio reported as published media");

        std::uint16_t video_sequence = 1;
        const auto units = nal_units(h264_idr);
        require(units.size() == 3, "h264 fixture split failed");
        for (std::uint32_t frame = 0; frame < 5; ++frame)
        {
            for (std::size_t index = 0; index < units.size(); ++index)
            {
                const auto packet = rtp(96, index + 1 == units.size(), video_sequence++, frame * 3'600, units[index]);
                require(media.input_packet(video_channel, packet), "video rtp rejected");
            }
        }
        send_audio(20);
        require(stream_registry::instance().find(name) != nullptr, "stream not registered after in-band video config");
        require(notifications != 0, "published media not reported after ready");
        media.shutdown();
        std::cout << "rtsp pull unready audio: PASS\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
