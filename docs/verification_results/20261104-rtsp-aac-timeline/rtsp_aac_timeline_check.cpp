#include <cmath>
#include <array>
#include <memory>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <algorithm>

#include <spdlog/spdlog.h>

#include "media/core/media_sink.h"
#include "media/core/media_stream.h"
#include "media/core/stream_registry.h"
#include "media/net/worker_context.h"
#include "media/rtsp/rtsp_sdp.h"
#include "media/rtsp/rtsp_pull_media.h"
#include "media/rtsp/rtsp_publish_media.h"

namespace
{

using media_server::media_kind;
using media_server::media_sink;
using media_server::media_frame;
using media_server::worker_context;
using media_server::rtsp_pull_media;
using media_server::stream_registry;
using media_server::rtsp_publish_media;
using media_server::rtsp_pull_track_description;
using media_server::rtsp_publish_track_description;

constexpr int payload_type = 97;
constexpr std::uint32_t ssrc = 0x11223344;
constexpr int samples_per_au = 1024;

struct collector final : media_sink
{
    explicit collector(worker_context& value) : worker_(value) {}
    worker_context& worker() noexcept override { return worker_; }
    void on_frame(const media_frame& frame) override
    {
        if (frame.track == 2)
        {
            frames.push_back(frame);
        }
    }
    void on_end() override {}

    worker_context& worker_;
    std::vector<media_frame> frames;
};

struct sent_au
{
    std::uint32_t index{};
    std::int64_t rtp_offset{};
};

struct scenario
{
    std::string name;
    int clock_rate{};
    int aus_per_packet{};
    int packets{};
    std::uint32_t first_timestamp{};
    std::vector<int> lost_packets{};
    int jump_packet{-1};
    std::uint32_t jump_ticks{};
    int sender_report_packet{-1};
};

std::string fmtp_for(int clock_rate)
{
    const auto config = clock_rate == 48'000 ? "1190" : "1210";
    return "streamtype=5; profile-level-id=1; mode=AAC-hbr; sizelength=13; indexlength=3; indexdeltalength=3; config=" + std::string(config);
}

std::vector<std::uint8_t> rtp_packet(std::uint16_t sequence, std::uint32_t timestamp, std::uint32_t first_index, int count)
{
    constexpr std::size_t au_bytes = 48;
    std::vector<std::uint8_t> packet{0x80,
                                     static_cast<std::uint8_t>(0x80U | payload_type),
                                     static_cast<std::uint8_t>(sequence >> 8U),
                                     static_cast<std::uint8_t>(sequence),
                                     static_cast<std::uint8_t>(timestamp >> 24U),
                                     static_cast<std::uint8_t>(timestamp >> 16U),
                                     static_cast<std::uint8_t>(timestamp >> 8U),
                                     static_cast<std::uint8_t>(timestamp),
                                     static_cast<std::uint8_t>(ssrc >> 24U),
                                     static_cast<std::uint8_t>(ssrc >> 16U),
                                     static_cast<std::uint8_t>(ssrc >> 8U),
                                     static_cast<std::uint8_t>(ssrc)};
    const auto header_bits = static_cast<std::uint16_t>(16 * count);
    packet.push_back(static_cast<std::uint8_t>(header_bits >> 8U));
    packet.push_back(static_cast<std::uint8_t>(header_bits));
    for (int au = 0; au < count; ++au)
    {
        const auto header = static_cast<std::uint16_t>(au_bytes << 3U);
        packet.push_back(static_cast<std::uint8_t>(header >> 8U));
        packet.push_back(static_cast<std::uint8_t>(header));
    }
    for (int au = 0; au < count; ++au)
    {
        const auto index = first_index + static_cast<std::uint32_t>(au);
        packet.push_back(static_cast<std::uint8_t>(index >> 24U));
        packet.push_back(static_cast<std::uint8_t>(index >> 16U));
        packet.push_back(static_cast<std::uint8_t>(index >> 8U));
        packet.push_back(static_cast<std::uint8_t>(index));
        packet.insert(packet.end(), au_bytes - 4, 0x5A);
    }
    return packet;
}

std::vector<std::uint8_t> sender_report(std::uint32_t timestamp, std::uint64_t ntp)
{
    std::vector<std::uint8_t> packet(28, 0);
    packet[0] = 0x80;
    packet[1] = 200;
    packet[3] = 6;
    for (std::size_t byte = 0; byte < 4; ++byte)
    {
        packet[4 + byte] = static_cast<std::uint8_t>(ssrc >> (24U - 8U * byte));
        packet[16 + byte] = static_cast<std::uint8_t>(timestamp >> (24U - 8U * byte));
    }
    for (std::size_t byte = 0; byte < 8; ++byte)
    {
        packet[8 + byte] = static_cast<std::uint8_t>(ntp >> (56U - 8U * byte));
    }
    return packet;
}

std::uint32_t frame_index(const media_frame& frame)
{
    const auto& data = *frame.payload;
    std::size_t offset = 0;
    if (data.size() >= 7 && data[0] == 0xFF && (data[1] & 0xF0U) == 0xF0U)
    {
        offset = (data[1] & 0x01U) != 0 ? 7 : 9;
    }
    if (data.size() < offset + 4)
    {
        return UINT32_MAX;
    }
    return (static_cast<std::uint32_t>(data[offset]) << 24U) | (static_cast<std::uint32_t>(data[offset + 1]) << 16U) |
           (static_cast<std::uint32_t>(data[offset + 2]) << 8U) | data[offset + 3];
}

template <typename Input>
std::vector<sent_au> drive(const scenario& value, Input&& input)
{
    std::vector<sent_au> sent;
    std::uint32_t timestamp = value.first_timestamp;
    std::int64_t offset = 0;
    std::uint32_t index = 0;
    for (int packet = 0; packet < value.packets; ++packet)
    {
        if (packet == value.jump_packet)
        {
            timestamp += value.jump_ticks;
            offset += value.jump_ticks;
        }
        if (packet == value.sender_report_packet)
        {
            const auto ntp = (static_cast<std::uint64_t>(3'900'000'000U) << 32U) +
                             static_cast<std::uint64_t>((static_cast<double>(offset) / value.clock_rate) * 4294967296.0);
            input(true, sender_report(timestamp, ntp));
        }
        const bool lost = std::ranges::find(value.lost_packets, packet) != value.lost_packets.end();
        if (!lost)
        {
            input(false, rtp_packet(static_cast<std::uint16_t>(1000 + packet), timestamp, index, value.aus_per_packet));
            for (int au = 0; au < value.aus_per_packet; ++au)
            {
                sent.push_back(sent_au{.index = index + static_cast<std::uint32_t>(au), .rtp_offset = offset + std::int64_t{au} * samples_per_au});
            }
        }
        const auto ticks = static_cast<std::uint32_t>(value.aus_per_packet * samples_per_au);
        timestamp += ticks;
        offset += ticks;
        index += static_cast<std::uint32_t>(value.aus_per_packet);
    }
    return sent;
}

int failures = 0;

void evaluate(const std::string& path, const scenario& value, const std::vector<sent_au>& sent, const std::vector<media_frame>& frames)
{
    const double au_ms = 1'000.0 * samples_per_au / value.clock_rate;
    std::size_t matched = 0;
    std::size_t content_errors = 0;
    std::size_t over_one_ms = 0;
    double error_min = 0;
    double error_max = 0;
    double step_error_max = 0;
    std::size_t sent_position = 0;
    const std::int64_t origin = frames.empty() ? 0 : frames.front().pts_ns;
    const std::int64_t origin_offset = sent.empty() ? 0 : sent.front().rtp_offset;
    for (std::size_t position = 0; position < frames.size(); ++position)
    {
        const auto index = frame_index(frames[position]);
        while (sent_position < sent.size() && sent[sent_position].index != index)
        {
            ++sent_position;
        }
        if (sent_position == sent.size())
        {
            ++content_errors;
            sent_position = 0;
            continue;
        }
        ++matched;
        const double expected_ms = static_cast<double>(sent[sent_position].rtp_offset - origin_offset) * 1'000.0 / value.clock_rate;
        const double actual_ms = static_cast<double>(frames[position].pts_ns - origin) / 1e6;
        const double error = actual_ms - expected_ms;
        error_min = std::min(error_min, error);
        error_max = std::max(error_max, error);
        over_one_ms += std::fabs(error) > 1.0 ? 1 : 0;
        if (position > 0 && frame_index(frames[position - 1]) + 1 == index)
        {
            const double step = static_cast<double>(frames[position].pts_ns - frames[position - 1].pts_ns) / 1e6;
            step_error_max = std::max(step_error_max, std::fabs(step - au_ms));
        }
        ++sent_position;
    }
    if (matched != sent.size() && std::getenv("CHECK_VERBOSE") != nullptr)
    {
        std::vector<std::uint32_t> received;
        for (const auto& frame : frames)
        {
            received.push_back(frame_index(frame));
        }
        for (const auto& au : sent)
        {
            if (std::ranges::find(received, au.index) == received.end())
            {
                std::printf("     missing au %u\n", au.index);
            }
        }
    }
    const bool ok = matched == sent.size() && content_errors == 0 && over_one_ms == 0;
    failures += ok ? 0 : 1;
    std::printf("%s %-7s %-22s frames %zu/%zu content_errors %zu pts_error_ms [%.3f, %.3f] over_1ms %zu step_error_max_ms %.3f\n",
                ok ? "ok  " : "FAIL",
                path.c_str(),
                value.name.c_str(),
                matched,
                sent.size(),
                content_errors,
                error_min,
                error_max,
                over_one_ms,
                step_error_max);
}

void drain(worker_context& worker)
{
    worker.io().restart();
    worker.io().poll();
}

void run_publish(worker_context& worker, const scenario& value, int serial)
{
    const auto fmtp = fmtp_for(value.clock_rate);
    auto track = media_server::rtsp_sdp_track_from_format("audio", payload_type, value.clock_rate, "MPEG4-GENERIC", fmtp.c_str(), 2);
    if (!track)
    {
        std::printf("FAIL publish %s sdp track\n", value.name.c_str());
        ++failures;
        return;
    }
    const auto stream_id = "publish-" + std::to_string(serial);
    rtsp_publish_media media(worker,
                             stream_id,
                             {rtsp_publish_track_description{.uri = "trackID=1",
                                                             .track = *track,
                                                             .clock_rate = value.clock_rate,
                                                             .payload_type = payload_type,
                                                             .encoding = "MPEG4-GENERIC",
                                                             .fmtp = fmtp}});
    auto sink = std::make_shared<collector>(worker);
    if (!media.startup("check") || !media.start_recording())
    {
        std::printf("FAIL publish %s startup\n", value.name.c_str());
        ++failures;
        return;
    }
    stream_registry::instance().find(stream_id)->add_sink(sink);
    drain(worker);
    const auto sent = drive(value,
                            [&](bool, const std::vector<std::uint8_t>& packet)
                            {
                                static_cast<void>(media.input_packet(0, packet));
                                drain(worker);
                            });
    evaluate("publish", value, sent, sink->frames);
    media.shutdown();
    drain(worker);
}

void run_pull(worker_context& worker, const scenario& value, int serial)
{
    const auto fmtp = fmtp_for(value.clock_rate);
    const std::string video_fmtp = "packetization-mode=1; sprop-parameter-sets=Z0LAC9woR+XARAAAAwAEAAADAMg8UK4=,aM4PLIA=";
    const auto stream_id = "pull-" + std::to_string(serial);
    rtsp_pull_media media(worker, stream_id);
    std::vector<rtsp_pull_track_description> descriptions{
        rtsp_pull_track_description{
            .kind = media_kind::video,
            .clock_rate = 90'000,
            .payload_type = 96,
            .encoding = "H264",
            .fmtp = video_fmtp,
            .initial_track = media_server::rtsp_sdp_track_from_format("video", 96, 90'000, "H264", video_fmtp.c_str(), 1),
        },
        rtsp_pull_track_description{
            .kind = media_kind::audio,
            .clock_rate = value.clock_rate,
            .payload_type = payload_type,
            .encoding = "MPEG4-GENERIC",
            .fmtp = fmtp,
            .initial_track = media_server::rtsp_sdp_track_from_format("audio", payload_type, value.clock_rate, "MPEG4-GENERIC", fmtp.c_str(), 2),
        },
    };
    auto sink = std::make_shared<collector>(worker);
    if (!descriptions[0].initial_track || !descriptions[1].initial_track || !media.startup(std::move(descriptions)) ||
        media.set_rtp_info(1, 1000, value.first_timestamp) != 0)
    {
        std::printf("FAIL pull %s startup\n", value.name.c_str());
        ++failures;
        return;
    }
    const std::vector<std::uint8_t> video_packet{0x80, 96, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0x06, 0x05, 0x01, 0x00, 0x80};
    auto stream = media.input_packet(0, video_packet) ? stream_registry::instance().find(stream_id) : nullptr;
    if (!stream)
    {
        std::printf("FAIL pull %s register\n", value.name.c_str());
        ++failures;
        return;
    }
    stream->add_sink(sink);
    drain(worker);
    const auto sent = drive(value,
                            [&](bool rtcp, const std::vector<std::uint8_t>& packet)
                            {
                                static_cast<void>(media.input_packet(rtcp ? 3 : 2, packet));
                                drain(worker);
                            });
    evaluate("pull", value, sent, sink->frames);
    media.shutdown();
    drain(worker);
}

}    // namespace

int main()
{
    spdlog::set_level(spdlog::level::off);
    worker_context worker;
    const std::vector<scenario> scenarios{
        {.name = "48k-1au", .clock_rate = 48'000, .aus_per_packet = 1, .packets = 400, .first_timestamp = 1'000, .lost_packets = {}},
        {.name = "48k-33au", .clock_rate = 48'000, .aus_per_packet = 33, .packets = 12, .first_timestamp = 1'000, .lost_packets = {}},
        {.name = "44k1-1au", .clock_rate = 44'100, .aus_per_packet = 1, .packets = 400, .first_timestamp = 1'000, .lost_packets = {}},
        {.name = "44k1-33au", .clock_rate = 44'100, .aus_per_packet = 33, .packets = 12, .first_timestamp = 1'000, .lost_packets = {}},
        {.name = "48k-33au-wrap",
         .clock_rate = 48'000,
         .aus_per_packet = 33,
         .packets = 12,
         .first_timestamp = 0xFFFFFFFFU - 3U * 33U * 1024U,
         .lost_packets = {}},
        {.name = "48k-1au-wrap",
         .clock_rate = 48'000,
         .aus_per_packet = 1,
         .packets = 400,
         .first_timestamp = 0xFFFFFFFFU - 100U * 1024U,
         .lost_packets = {}},
        {.name = "48k-1au-loss", .clock_rate = 48'000, .aus_per_packet = 1, .packets = 400, .first_timestamp = 1'000, .lost_packets = {50, 51, 200}},
        {.name = "48k-33au-loss", .clock_rate = 48'000, .aus_per_packet = 33, .packets = 12, .first_timestamp = 1'000, .lost_packets = {4}},
        {.name = "48k-1au-jump",
         .clock_rate = 48'000,
         .aus_per_packet = 1,
         .packets = 400,
         .first_timestamp = 1'000,
         .lost_packets = {},
         .jump_packet = 200,
         .jump_ticks = 48'000},
        {.name = "48k-33au-jump",
         .clock_rate = 48'000,
         .aus_per_packet = 33,
         .packets = 12,
         .first_timestamp = 1'000,
         .lost_packets = {},
         .jump_packet = 6,
         .jump_ticks = 48'000},
        {.name = "48k-1au-sr",
         .clock_rate = 48'000,
         .aus_per_packet = 1,
         .packets = 400,
         .first_timestamp = 1'000,
         .lost_packets = {},
         .sender_report_packet = 100},
        {.name = "48k-33au-sr",
         .clock_rate = 48'000,
         .aus_per_packet = 33,
         .packets = 12,
         .first_timestamp = 1'000,
         .lost_packets = {},
         .sender_report_packet = 3},
    };
    int serial = 0;
    for (const auto& value : scenarios)
    {
        run_publish(worker, value, ++serial);
        run_pull(worker, value, ++serial);
    }
    std::printf("%s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
