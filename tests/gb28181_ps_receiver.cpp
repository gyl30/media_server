#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "media/core/stream_registry.h"
#include "media/net/worker_context.h"
#include "media/gb28181/gb28181_rtp_receiver.h"

extern "C"
{
#include "mpeg-ps.h"
#include "mpeg-proto.h"
#include "rtp-payload.h"
}

namespace
{
using namespace media_server;

constexpr std::uint8_t payload_type = 96;
constexpr std::uint32_t ssrc = 1'234'567'890;

// 16x16 H.264 SPS/PPS/IDR，由 FFmpeg libx264 生成。
constexpr std::array<std::uint8_t, 45> h264_idr{ 0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0xc0, 0x0a, 0xdd, 0xec, 0x04, 0x40, 0x00, 0x00, 0x03, 0x00, 0x40, 0x00, 0x00, 0x0c, 0x83, 0xc4, 0x89, 0xe0, 0x00, 0x00, 0x00, 0x01, 0x68, 0xce, 0x0f, 0xc8, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x3a, 0x26, 0x28, 0x00, 0x09, 0x02, 0xe0};

void require(bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

struct ps_writer
{
    std::vector<std::uint8_t> buffer;
    std::vector<std::vector<std::uint8_t>> packets;

    static void* alloc(void* param, std::size_t bytes)
    {
        auto* self = static_cast<ps_writer*>(param);
        self->buffer.resize(bytes);
        return self->buffer.data();
    }
    static void free(void*, void*) {}
    static int write(void* param, int, void* packet, std::size_t bytes)
    {
        auto* self = static_cast<ps_writer*>(param);
        const auto* begin = static_cast<const std::uint8_t*>(packet);
        self->packets.emplace_back(begin, begin + bytes);
        return 0;
    }
};

struct rtp_writer
{
    std::vector<std::uint8_t> buffer;
    std::vector<std::vector<std::uint8_t>> packets;

    static void* alloc(void* param, int bytes)
    {
        auto* self = static_cast<rtp_writer*>(param);
        self->buffer.resize(static_cast<std::size_t>(bytes));
        return self->buffer.data();
    }
    static void free(void*, void*) {}
    static int packet(void* param, const void* data, int bytes, std::uint32_t, int)
    {
        auto* self = static_cast<rtp_writer*>(param);
        const auto* begin = static_cast<const std::uint8_t*>(data);
        self->packets.emplace_back(begin, begin + bytes);
        return 0;
    }
};

std::vector<std::vector<std::uint8_t>> make_rtp(int audio_codec, std::size_t audio_bytes)
{
    ps_writer ps;
    const ps_muxer_func_t ps_functions{&ps_writer::alloc, &ps_writer::free, &ps_writer::write};
    auto* muxer = ps_muxer_create(&ps_functions, &ps);
    const auto video = ps_muxer_add_stream(muxer, PSI_STREAM_H264, nullptr, 0);
    const auto audio = ps_muxer_add_stream(muxer, audio_codec, nullptr, 0);
    const std::vector<std::uint8_t> audio_frame(audio_bytes, 0x55);
    for (std::int64_t index = 0; index < 10; ++index)
    {
        const auto pts = index * 3'600;
        require(ps_muxer_input(muxer, video, MPEG_FLAG_IDR_FRAME, pts, pts, h264_idr.data(), h264_idr.size()) == 0, "ps video mux failed");
        require(ps_muxer_input(muxer, audio, 0, pts, pts, audio_frame.data(), audio_frame.size()) == 0, "ps audio mux failed");
    }
    ps_muxer_destroy(muxer);

    rtp_writer rtp;
    rtp_payload_t rtp_functions{&rtp_writer::alloc, &rtp_writer::free, &rtp_writer::packet};
    auto* encoder = rtp_payload_encode_create(payload_type, "PS", 1, ssrc, &rtp_functions, &rtp);
    std::uint32_t timestamp = 0;
    for (const auto& packet : ps.packets)
    {
        require(rtp_payload_encode_input(encoder, packet.data(), static_cast<int>(packet.size()), timestamp) == 0, "rtp encode failed");
        timestamp += 3'600;
    }
    rtp_payload_encode_destroy(encoder);
    return rtp.packets;
}

void check(const char* name, int audio_codec, std::size_t audio_bytes, std::size_t expected_tracks)
{
    worker_context worker;
    const std::string stream_name = std::string("gb/test/") + name;
    gb28181_rtp_receiver receiver(worker, stream_name, payload_type, ssrc);
    require(receiver.startup(), std::string(name) + ": receiver startup failed");
    for (const auto& packet : make_rtp(audio_codec, audio_bytes))
    {
        require(receiver.receive_rtp(packet) != gb28181_rtp_receive_result::fatal, std::string(name) + ": stream rejected");
    }
    const auto stream = stream_registry::instance().find(stream_name);
    require(stream && stream->tracks().size() == expected_tracks && stream->tracks().front().codec == codec_id::h264,
            std::string(name) + ": unexpected tracks");
    receiver.shutdown();
    std::cout << name << ": PASS\n";
}
}    // namespace

int main()
{
    try
    {
        check("g711a", PSI_STREAM_AUDIO_G711A, 320, 2);
        // 不支持的音频降级为纯视频，不能让整路视频失败。
        check("g722", PSI_STREAM_AUDIO_G722, 160, 1);
        check("g729", PSI_STREAM_AUDIO_G729, 20, 1);
        check("mp3", PSI_STREAM_MP3, 144, 1);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
