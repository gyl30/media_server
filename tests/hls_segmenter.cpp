#include <array>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "media/codec/codec_utils.h"
#include "media/core/media_stream.h"
#include "media/hls/hls_segmenter.h"
#include "media/net/worker_context.h"

namespace
{
using namespace media_server;

// 16x16 H.264 SPS/PPS/IDR，由 FFmpeg libx264 生成。
constexpr std::array<std::uint8_t, 45> h264_idr{ 0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0xc0, 0x0a, 0xdd, 0xec, 0x04, 0x40, 0x00, 0x00, 0x03, 0x00, 0x40, 0x00, 0x00, 0x0c, 0x83, 0xc4, 0x89, 0xe0, 0x00, 0x00, 0x00, 0x01, 0x68, 0xce, 0x0f, 0xc8, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x3a, 0x26, 0x28, 0x00, 0x09, 0x02, 0xe0};
const std::vector<std::uint8_t> aac_config{0x11, 0x90};

void require(bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

media_frame make_frame(track_id track, std::int64_t pts_ns, bool key_frame, std::vector<std::uint8_t> payload)
{
    return media_frame{.track = track,
                       .dts_ns = pts_ns,
                       .pts_ns = pts_ns,
                       .key_frame = key_frame,
                       .payload = std::make_shared<const std::vector<std::uint8_t>>(std::move(payload))};
}

std::shared_ptr<hls_segmenter> start(worker_context& worker, std::vector<media_track> tracks)
{
    auto stream = std::make_shared<media_stream>("hls/test", worker);
    require(stream->set_tracks(std::move(tracks)), "tracks rejected");
    auto segmenter = std::make_shared<hls_segmenter>();
    return segmenter->startup(stream) ? segmenter : nullptr;
}

void video_with_opus(worker_context& worker)
{
    const auto segmenter = start(worker,
                                 {{.id = 1, .kind = media_kind::video, .codec = codec_id::h264, .clock_rate = 90'000, .codec_config = {}},
                                  {.id = 2, .kind = media_kind::audio, .codec = codec_id::opus, .clock_rate = 48'000, .channel_count = 2, .codec_config = {}}});
    require(segmenter != nullptr, "video with opus: opus track disabled whole hls");
    for (std::int64_t index = 0; index < 100; ++index)
    {
        const auto pts = index * 40'000'000;
        segmenter->on_frame(make_frame(1, pts, true, {h264_idr.begin(), h264_idr.end()}));
        segmenter->on_frame(make_frame(2, pts, false, std::vector<std::uint8_t>(40, 0x11)));
    }
    require(segmenter->has_segments(), "video with opus: no segments");
    segmenter->shutdown();
    std::cout << "video_with_opus: PASS\n";
}

void audio_only(worker_context& worker)
{
    const auto segmenter = start(worker, {{.id = 2, .kind = media_kind::audio, .codec = codec_id::aac, .clock_rate = 48'000,
                                           .channel_count = 2, .codec_config = aac_config}});
    require(segmenter != nullptr, "audio only: startup failed");
    const auto frame = make_adts_frame(aac_config, std::vector<std::uint8_t>(32, 0x00));
    require(!frame.empty(), "audio only: adts fixture failed");
    for (std::int64_t index = 0; index < 250; ++index)
    {
        segmenter->on_frame(make_frame(2, index * 21'333'333, false, frame));
    }
    require(segmenter->has_segments(), "audio only: never segmented");
    segmenter->shutdown();
    std::cout << "audio_only: PASS\n";
}

void opus_only(worker_context& worker)
{
    auto stream = std::make_shared<media_stream>("hls/test", worker);
    require(stream->set_tracks({{.id = 2, .kind = media_kind::audio, .codec = codec_id::opus, .clock_rate = 48'000, .channel_count = 2, .codec_config = {}}}),
            "tracks rejected");
    const auto segmenter = std::make_shared<hls_segmenter>();
    require(!segmenter->startup(stream), "opus only: unsupported stream accepted");
    // 启动失败后由调用方 shutdown 清理，此时 muxer 尚未创建。
    segmenter->shutdown();
    std::cout << "opus_only: PASS\n";
}
}    // namespace

int main()
{
    try
    {
        worker_context worker;
        video_with_opus(worker);
        audio_only(worker);
        opus_only(worker);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
