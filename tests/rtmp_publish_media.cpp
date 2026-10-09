#include <array>
#include <cstring>
#include <memory>
#include <utility>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "media/codec/codec_utils.h"
#include "media/core/media_sink.h"
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

// AMF0 onMetaData {audiocodecid: codec}；0 表示无音频。
std::vector<std::uint8_t> metadata(double audio_codec)
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
    std::uint64_t bits{};
    std::memcpy(&bits, &audio_codec, sizeof(bits));
    for (int shift = 56; shift >= 0; shift -= 8)
    {
        data.push_back(static_cast<std::uint8_t>(bits >> static_cast<unsigned>(shift)));
    }
    data.insert(data.end(), {0x00, 0x00, 0x09});
    return data;
}

// 把 Annex-B 访问单元（3 或 4 字节起始码）转成 AVCC 长度前缀格式。
std::vector<std::uint8_t> annex_b_to_length_prefixed(std::span<const std::uint8_t> annex_b)
{
    std::vector<std::pair<std::size_t, std::size_t>> units;
    std::size_t index = 0;
    while (index + 3 <= annex_b.size())
    {
        if (annex_b[index] == 0 && annex_b[index + 1] == 0 && annex_b[index + 2] == 1)
        {
            if (!units.empty())
            {
                const auto end = index > 0 && annex_b[index - 1] == 0 ? index - 1 : index;
                units.back().second = end;
            }
            units.emplace_back(index + 3, annex_b.size());
            index += 3;
            continue;
        }
        ++index;
    }
    std::vector<std::uint8_t> result;
    for (const auto& [begin, end] : units)
    {
        const auto bytes = end - begin;
        result.insert(result.end(), {static_cast<std::uint8_t>(bytes >> 24U), static_cast<std::uint8_t>(bytes >> 16U),
                                     static_cast<std::uint8_t>(bytes >> 8U), static_cast<std::uint8_t>(bytes)});
        result.insert(result.end(), annex_b.begin() + static_cast<std::ptrdiff_t>(begin), annex_b.begin() + static_cast<std::ptrdiff_t>(end));
    }
    return result;
}

std::vector<std::uint8_t> video_tag(std::uint8_t packet_type, std::span<const std::uint8_t> body)
{
    std::vector<std::uint8_t> tag{0x17, packet_type, 0x00, 0x00, 0x00};
    tag.insert(tag.end(), body.begin(), body.end());
    return tag;
}

class frame_sink final : public media_sink
{
   public:
    explicit frame_sink(worker_context& worker) : worker_(worker) {}

    worker_context& worker() noexcept override { return worker_; }
    void on_frame(const media_frame& frame) override { frames.push_back(frame); }
    void on_end() override {}

    std::vector<media_frame> frames;

   private:
    worker_context& worker_;
};

// 发布会话、下游 sink 与发布出的帧计数。
struct publisher
{
    worker_context worker;
    std::size_t notifications{};
    rtmp_publish_session session;
    std::shared_ptr<frame_sink> sink = std::make_shared<frame_sink>(worker);

    explicit publisher(const std::string& name) : session(worker, name, [this]() { ++notifications; }) {}

    void register_video(double audio_codec)
    {
        require(session.startup(), "publish startup failed");
        const auto data = metadata(audio_codec);
        require(session.on_script(data) == 0, "metadata rejected");
        const auto avcc = h264_annex_b_to_avcc(h264_idr);
        require(!avcc.empty(), "avcc fixture failed");
        const auto header = video_tag(0x00, avcc);
        require(session.on_video(header.data(), header.size(), 0) == 0, "sequence header rejected");
    }

    void attach(const std::string& name)
    {
        const auto stream = stream_registry::instance().find(name);
        require(stream != nullptr, "stream not registered");
        stream->add_sink(sink);
        worker.io().poll();
    }
};

void video_messages()
{
    publisher publish("rtmp/video");
    publish.register_video(0);
    publish.attach("rtmp/video");
    require(publish.notifications == 0, "sequence header reported as media");

    // AVC end-of-sequence 被解复用器忽略，不是媒体。
    const auto end_of_sequence = video_tag(0x02, {});
    for (std::uint32_t timestamp = 40; timestamp < 400; timestamp += 40)
    {
        require(publish.session.on_video(end_of_sequence.data(), end_of_sequence.size(), timestamp) == 0, "end of sequence rejected");
    }
    require(publish.notifications == 0 && publish.sink->frames.empty(), "end of sequence reported as media");

    const auto frame = video_tag(0x01, annex_b_to_length_prefixed(h264_idr));
    require(publish.session.on_video(frame.data(), frame.size(), 400) == 0, "video frame rejected");
    require(publish.notifications == 1 && publish.sink->frames.size() == 1 && publish.sink->frames.front().key_frame &&
                publish.sink->frames.front().payload->size() > h264_idr.size(),
            "video frame not published as media");
    publish.session.shutdown();
    std::cout << "video_messages: PASS\n";
}

void empty_g711_audio()
{
    publisher publish("rtmp/g711");
    // FLV audiocodecid 7 为 G.711 A-law；首条带数据的 G.711 消息建立音轨并完成登记。
    publish.register_video(7);
    std::array<std::uint8_t, 161> audio{};
    audio.front() = 0x70;
    require(publish.session.on_audio(audio.data(), audio.size(), 0) == 0, "g711 frame rejected");
    publish.attach("rtmp/g711");
    const auto before = publish.notifications;

    const std::array<std::uint8_t, 1> empty_audio{0x70};
    for (std::uint32_t timestamp = 20; timestamp < 400; timestamp += 20)
    {
        require(publish.session.on_audio(empty_audio.data(), empty_audio.size(), timestamp) == 0, "empty g711 rejected");
    }
    require(publish.notifications == before && publish.sink->frames.empty(), "empty g711 reported as media");

    require(publish.session.on_audio(audio.data(), audio.size(), 400) == 0, "g711 frame rejected");
    require(publish.notifications == before + 1 && publish.sink->frames.size() == 1 && publish.sink->frames.front().payload->size() == 160,
            "g711 frame not published as media");
    publish.session.shutdown();
    std::cout << "empty_g711_audio: PASS\n";
}

void empty_aac_audio()
{
    publisher publish("rtmp/aac");
    // FLV audiocodecid 10 为 AAC。
    publish.register_video(10);
    const std::array<std::uint8_t, 4> sequence_header{0xaf, 0x00, 0x11, 0x90};
    require(publish.session.on_audio(sequence_header.data(), sequence_header.size(), 0) == 0, "aac sequence header rejected");
    publish.attach("rtmp/aac");

    // 只有 AAC 标签头、没有 ES 的消息；解复用器会为它补出 7 字节 ADTS 头。
    const std::array<std::uint8_t, 2> empty_audio{0xaf, 0x01};
    for (std::uint32_t timestamp = 20; timestamp < 400; timestamp += 20)
    {
        require(publish.session.on_audio(empty_audio.data(), empty_audio.size(), timestamp) == 0, "empty aac rejected");
    }
    require(publish.notifications == 0 && publish.sink->frames.empty(), "empty aac reported as media");

    std::array<std::uint8_t, 22> audio{};
    audio[0] = 0xaf;
    audio[1] = 0x01;
    require(publish.session.on_audio(audio.data(), audio.size(), 400) == 0, "aac frame rejected");
    require(publish.notifications == 1 && publish.sink->frames.size() == 1 && publish.sink->frames.front().payload->size() == 7 + 20,
            "aac frame not published as media");
    publish.session.shutdown();
    std::cout << "empty_aac_audio: PASS\n";
}
}    // namespace

int main()
{
    try
    {
        video_messages();
        empty_g711_audio();
        empty_aac_audio();
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
