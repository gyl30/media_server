#include <array>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "media/codec/codec_utils.h"
#include "media/core/stream_registry.h"
#include "media/net/worker_context.h"
#include "media/rtmp/rtmp_publish_session.h"

namespace
{
using namespace media_server;

// 16x16 H.264 SPS/PPS/IDR，由 FFmpeg libx264 生成。
constexpr std::array<std::uint8_t, 45> h264_idr{ 0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0xc0, 0x0a, 0xdd, 0xec, 0x04, 0x40, 0x00, 0x00, 0x03, 0x00, 0x40, 0x00, 0x00, 0x0c, 0x83, 0xc4, 0x89, 0xe0, 0x00, 0x00, 0x00, 0x01, 0x68, 0xce, 0x0f, 0xc8, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x3a, 0x26, 0x28, 0x00, 0x09, 0x02, 0xe0};

void require(bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

// AMF0 onMetaData {audiocodecid: 0}，声明无音频。
std::vector<std::uint8_t> metadata_without_audio()
{
    std::vector<std::uint8_t> data{0x02, 0x00, 0x0a};
    const std::string name = "onMetaData";
    data.insert(data.end(), name.begin(), name.end());
    data.push_back(0x03);
    const std::string key = "audiocodecid";
    data.push_back(0x00);
    data.push_back(static_cast<std::uint8_t>(key.size()));
    data.insert(data.end(), key.begin(), key.end());
    data.push_back(0x00);
    data.insert(data.end(), 8, 0x00);
    data.insert(data.end(), {0x00, 0x00, 0x09});
    return data;
}

// 把 Annex-B 访问单元转成 AVCC 长度前缀格式。
std::vector<std::uint8_t> annex_b_to_length_prefixed(std::span<const std::uint8_t> annex_b)
{
    std::vector<std::size_t> starts;
    for (std::size_t index = 0; index + 4 <= annex_b.size(); ++index)
    {
        if (annex_b[index] == 0 && annex_b[index + 1] == 0 && annex_b[index + 2] == 0 && annex_b[index + 3] == 1)
        {
            starts.push_back(index + 4);
        }
    }
    std::vector<std::uint8_t> result;
    for (std::size_t index = 0; index < starts.size(); ++index)
    {
        const auto end = index + 1 < starts.size() ? starts[index + 1] - 4 : annex_b.size();
        const auto bytes = end - starts[index];
        result.insert(result.end(), {static_cast<std::uint8_t>(bytes >> 24U), static_cast<std::uint8_t>(bytes >> 16U),
                                     static_cast<std::uint8_t>(bytes >> 8U), static_cast<std::uint8_t>(bytes)});
        result.insert(result.end(), annex_b.begin() + static_cast<std::ptrdiff_t>(starts[index]), annex_b.begin() + static_cast<std::ptrdiff_t>(end));
    }
    return result;
}

std::vector<std::uint8_t> video_tag(std::uint8_t packet_type, std::span<const std::uint8_t> body)
{
    std::vector<std::uint8_t> tag{0x17, packet_type, 0x00, 0x00, 0x00};
    tag.insert(tag.end(), body.begin(), body.end());
    return tag;
}
}    // namespace

int main()
{
    try
    {
        worker_context worker;
        std::size_t frames{};
        rtmp_publish_session session(worker, "rtmp/media", [&frames]() { ++frames; });
        require(session.startup(), "publish startup failed");
        const auto metadata = metadata_without_audio();
        require(session.on_script(metadata) == 0, "metadata rejected");
        const auto avcc = h264_annex_b_to_avcc(h264_idr);
        require(!avcc.empty(), "avcc fixture failed");
        const auto header = video_tag(0x00, avcc);
        require(session.on_video(header.data(), header.size(), 0) == 0, "sequence header rejected");
        require(stream_registry::instance().find("rtmp/media") != nullptr, "stream not registered");
        require(frames == 0, "sequence header reported as media");

        // AVC end-of-sequence 被解复用器忽略，不是媒体。
        const auto end_of_sequence = video_tag(0x02, {});
        for (std::uint32_t timestamp = 40; timestamp < 400; timestamp += 40)
        {
            require(session.on_video(end_of_sequence.data(), end_of_sequence.size(), timestamp) == 0, "end of sequence rejected");
        }
        require(frames == 0, "end of sequence reported as media");

        const auto frame = video_tag(0x01, annex_b_to_length_prefixed(h264_idr));
        require(session.on_video(frame.data(), frame.size(), 400) == 0, "video frame rejected");
        require(frames == 1, "video frame not reported as media");
        session.shutdown();
        std::cout << "rtmp publish media: PASS\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
