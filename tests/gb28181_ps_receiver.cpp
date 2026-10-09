#include <array>
#include <functional>
#include <optional>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "media/codec/codec_utils.h"
#include "media/core/stream_registry.h"
#include "media/net/worker_context.h"
#include "media/gb28181/gb28181_rtp_receiver.h"

extern "C"
{
#include "mpeg-ps.h"
#include "mpeg-proto.h"
#include "mpeg-util.h"
#include "rtp-payload.h"
}

namespace
{
using namespace media_server;

constexpr std::uint8_t payload_type = 96;
constexpr std::uint32_t ssrc = 1'234'567'890;

// 16x16 H.264 SPS/PPS/IDR，由 FFmpeg libx264 生成。
constexpr std::array<std::uint8_t, 45> h264_idr{ 0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0xc0, 0x0a, 0xdd, 0xec, 0x04, 0x40, 0x00, 0x00, 0x03, 0x00, 0x40, 0x00, 0x00, 0x0c, 0x83, 0xc4, 0x89, 0xe0, 0x00, 0x00, 0x00, 0x01, 0x68, 0xce, 0x0f, 0xc8, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x3a, 0x26, 0x28, 0x00, 0x09, 0x02, 0xe0};

// 8 帧 MPEG-2 Layer III（16kHz、32kbps，每帧 144 字节），由 FFmpeg libmp3lame 生成。
constexpr std::array<std::uint8_t, 1152> mp3_frames{ 0xff, 0xf3, 0x48, 0xc4, 0x00, 0x1c, 0x78, 0x8e, 0x6c, 0x15, 0x58, 0x30, 0x00, 0x25, 0x5a, 0xf5, 0x4e, 0xa9, 0xd5, 0x3a, 0xa7, 0x54, 0xea, 0x9d, 0x77, 0xb1, 0x36, 0xbe, 0xb0, 0x88, 0x98, 0x5c, 0x84, 0xe8, 0x68, 0xe0, 0x13, 0x9b, 0x62, 0x73, 0x89, 0xb4, 0xa6, 0x81, 0x18, 0x00, 0x59, 0x82, 0xf0, 0x28, 0x23, 0x10, 0x72, 0x21, 0xc9, 0x61, 0x32, 0x60, 0x30, 0x18, 0x0c, 0x2c, 0x9a, 0x60, 0xe0, 0x20, 0x08, 0x02, 0x0e, 0x89, 0xc1, 0xfe, 0x51, 0xd3, 0x9d, 0x3e, 0x73, 0x97, 0xf3, 0x9c, 0xbf, 0xbb, 0xa7, 0xdd, 0x94, 0x04, 0xc1, 0xf3, 0xeb, 0x0f, 0x82, 0x01, 0x89, 0x40, 0x18, 0x3e, 0xf9, 0x30, 0x40, 0xe6, 0x5d, 0xfd, 0xdc, 0xb8, 0x3e, 0x0f, 0x83, 0xe1, 0xf0, 0x40, 0x10, 0x04, 0x01, 0x00, 0x18, 0x3e, 0x0f, 0x83, 0xe1, 0xf0, 0x40, 0x10, 0x04, 0x1c, 0x91, 0x38, 0x3e, 0x20, 0x09, 0x20, 0x81, 0x00, 0x68, 0x10, 0x60, 0x58, 0x1d, 0xbe, 0x7f, 0x98, 0x32, 0x15, 0x01, 0x01, 0xff, 0xf3, 0x48, 0xc4, 0x11, 0x20, 0xa1, 0xf6, 0x6c, 0x2d, 0x9d, 0xa0, 0x00, 0x9d, 0x0c, 0x80, 0x3e, 0x83, 0xa6, 0x23, 0x13, 0xe6, 0xbb, 0x25, 0x46, 0x0d, 0x15, 0x46, 0xc4, 0x1e, 0xa6, 0x11, 0x8b, 0xc7, 0x0c, 0x8c, 0x20, 0x62, 0xe0, 0x81, 0xa0, 0x1e, 0x06, 0x18, 0x58, 0x18, 0xa6, 0x20, 0xa0, 0xb2, 0x70, 0x0c, 0x96, 0x20, 0x1a, 0x0c, 0x5d, 0x0d, 0x98, 0x2d, 0x18, 0x2d, 0x08, 0x52, 0x3f, 0xe2, 0x51, 0x10, 0x54, 0x72, 0x85, 0xcc, 0x2e, 0x6f, 0xfc, 0x99, 0x1c, 0xd1, 0xcd, 0x26, 0x88, 0xb1, 0x16, 0xff, 0xf2, 0x64, 0x8a, 0x90, 0x13, 0x22, 0x2c, 0x45, 0x8c, 0x7f, 0xff, 0x2e, 0x97, 0x4c, 0x8b, 0xc5, 0xe4, 0x4b, 0xa5, 0xd4, 0xbf, 0xff, 0xf2, 0xf1, 0x79, 0x12, 0xe9, 0x74, 0x1a, 0x0a, 0x82, 0xac, 0xff, 0xe5, 0x82, 0xa1, 0xa2, 0xdf, 0xff, 0xff, 0xf5, 0x5a, 0xaa, 0xa0, 0x00, 0x00, 0x92, 0x08, 0xa1, 0x0f, 0xe7, 0xff, 0xf4, 0x0c, 0xe9, 0x4c, 0xa0, 0xb6, 0xf0, 0xc0, 0xff, 0xf3, 0x48, 0xc4, 0x11, 0x19, 0x98, 0x76, 0x26, 0x37, 0xde, 0x40, 0x00, 0x3c, 0x05, 0x0c, 0x09, 0x41, 0x90, 0xc2, 0x04, 0x05, 0x8c, 0x20, 0x83, 0x34, 0xc5, 0xb8, 0xef, 0x4e, 0x5d, 0x0c, 0x18, 0xfd, 0x94, 0xff, 0x0c, 0xae, 0xcb, 0x88, 0xc7, 0x58, 0x5a, 0xcc, 0x4d, 0x09, 0x1c, 0xc0, 0x0c, 0x3f, 0x4c, 0x1c, 0x80, 0x8c, 0xc0, 0x38, 0x00, 0x53, 0xb9, 0x58, 0xa1, 0xe8, 0x04, 0x2c, 0xa1, 0xad, 0xb7, 0x33, 0x39, 0xbb, 0xf6, 0x2f, 0xf5, 0xbf, 0xfe, 0xf2, 0x77, 0x57, 0xaf, 0xfd, 0xe5, 0xef, 0xe4, 0xd6, 0xee, 0xcf, 0xb9, 0xbb, 0x7f, 0x65, 0xc4, 0x3e, 0xea, 0x8f, 0xee, 0xee, 0xfd, 0x3d, 0x55, 0xff, 0xff, 0xd4, 0xa9, 0x41, 0x4c, 0x24, 0x18, 0xc8, 0x06, 0xcc, 0xd1, 0x1c, 0xcd, 0xdb, 0x8d, 0x0a, 0xac, 0xc0, 0x65, 0x04, 0x70, 0xc0, 0xbc, 0x08, 0xb4, 0xc1, 0xd3, 0x19, 0x50, 0xca, 0x74, 0xfe, 0xb8, 0xd6, 0x4b, 0x21, 0x28, 0xc1, 0x84, 0x07, 0x84, 0xc0, 0xd5, 0x01, 0xff, 0xf3, 0x48, 0xc4, 0x2d, 0x23, 0xe3, 0xba, 0x04, 0x00, 0xdf, 0xc4, 0x70, 0xc0, 0xc2, 0x8c, 0x0d, 0x1c, 0xc1, 0x0f, 0x04, 0xdc, 0xc0, 0xef, 0x01, 0x20, 0xc0, 0x50, 0x00, 0x54, 0xc0, 0x1f, 0x00, 0x00, 0x30, 0x00, 0xd4, 0xce, 0x6f, 0x24, 0x92, 0x9a, 0x6d, 0xfe, 0xb5, 0xd5, 0xa1, 0x9a, 0x17, 0x0f, 0x8f, 0x41, 0x7b, 0xff, 0x5c, 0xf4, 0x09, 0xbd, 0x4d, 0x47, 0xab, 0x54, 0x7a, 0xb5, 0xf4, 0xdf, 0x4f, 0x8f, 0x57, 0xab, 0x57, 0x26, 0x4c, 0x52, 0x74, 0x6d, 0xfd, 0x5a, 0x17, 0x26, 0xd8, 0xde, 0xba, 0xb5, 0x7e, 0x0f, 0x5d, 0xaa, 0x4a, 0x0d, 0x07, 0xa3, 0xd9, 0xe0, 0xb2, 0x6e, 0x9e, 0xf4, 0x6d, 0xab, 0xfe, 0xbb, 0x63, 0xfa, 0xeb, 0xf0, 0xd3, 0xa8, 0xc5, 0x6f, 0x76, 0xf5, 0x80, 0x00, 0x11, 0xfb, 0xff, 0xe4, 0xad, 0x61, 0x0b, 0xa6, 0x3c, 0x00, 0x28, 0xa0, 0x02, 0xe6, 0x00, 0x60, 0x08, 0x86, 0x01, 0xd8, 0x0f, 0xe6, 0x03, 0x90, 0x1b, 0x86, 0x06, 0xb8, 0x36, 0x26, 0xff, 0xf3, 0x48, 0xc4, 0x20, 0x21, 0x03, 0xbe, 0x0d, 0xe8, 0xff, 0x44, 0xa8, 0x0e, 0xd0, 0x99, 0x46, 0x4b, 0x5d, 0xe3, 0x46, 0x9f, 0x10, 0xcd, 0x86, 0x13, 0xe8, 0x46, 0xa6, 0xac, 0xac, 0x27, 0x07, 0x61, 0x46, 0x69, 0x9f, 0x46, 0x52, 0x85, 0xe6, 0x17, 0x00, 0xc1, 0x00, 0xda, 0x03, 0x15, 0xdb, 0x10, 0x7f, 0x23, 0x78, 0x5f, 0xff, 0x7a, 0xeb, 0xab, 0x53, 0x26, 0x4a, 0x8a, 0xff, 0xf9, 0x68, 0x7d, 0xb1, 0xb7, 0xdb, 0x1e, 0xba, 0xeb, 0xb7, 0xf5, 0x7a, 0xbd, 0x75, 0xcd, 0xae, 0x37, 0xa7, 0xfa, 0xe4, 0xcf, 0xbe, 0x7d, 0xaa, 0xd5, 0xd5, 0xeb, 0x93, 0xe7, 0xda, 0x9a, 0xe9, 0x82, 0xcf, 0x9f, 0x6f, 0xbd, 0x1a, 0xad, 0xff, 0xa7, 0xaf, 0xd7, 0x5f, 0x83, 0x96, 0xb5, 0x3a, 0x76, 0x15, 0xa0, 0x00, 0x00, 0xa3, 0xa9, 0x25, 0x17, 0xeb, 0xff, 0xee, 0xb9, 0x6c, 0x3c, 0x50, 0x01, 0x83, 0x00, 0xbc, 0xc0, 0x00, 0x02, 0x8c, 0x07, 0x01, 0x18, 0x14, 0x13, 0xa6, 0x11, 0xa1, 0xa6, 0xff, 0xf3, 0x48, 0xc4, 0x1f, 0x19, 0xd0, 0x76, 0x2a, 0x37, 0x5e, 0x18, 0x00, 0x62, 0xbe, 0x69, 0xe7, 0x03, 0x21, 0xd6, 0x7b, 0x88, 0x6f, 0xc6, 0x41, 0x42, 0x1c, 0x60, 0x84, 0x1e, 0x46, 0x22, 0x44, 0x18, 0x08, 0x0e, 0x83, 0x07, 0x60, 0x26, 0x01, 0x00, 0xa2, 0x77, 0xa9, 0x7b, 0xff, 0x92, 0x8b, 0x3f, 0xb5, 0x76, 0xa6, 0xfd, 0xdf, 0xb1, 0x17, 0xa7, 0x45, 0xed, 0xe9, 0xeb, 0x93, 0x5e, 0xbd, 0x3a, 0x7e, 0xf4, 0x7d, 0x0b, 0xf7, 0xfa, 0x9e, 0xd5, 0x4d, 0x22, 0xef, 0x55, 0xaa, 0xfe, 0xea, 0xfd, 0x88, 0xfa, 0x7a, 0x95, 0x04, 0x92, 0x49, 0x20, 0x52, 0x00, 0x03, 0x2b, 0x06, 0x0c, 0x01, 0x81, 0x50, 0x8d, 0x10, 0x92, 0x0b, 0xd6, 0x1c, 0xc2, 0x30, 0x1c, 0xc2, 0x90, 0xfc, 0x00, 0x08, 0x80, 0x00, 0xd3, 0x0a, 0x05, 0xc3, 0x87, 0x8a, 0x12, 0xc2, 0x24, 0x67, 0x58, 0xf2, 0x89, 0x89, 0x1e, 0x09, 0x4d, 0x10, 0x0c, 0xb8, 0x07, 0x04, 0x00, 0x04, 0x96, 0x01, 0x83, 0x80, 0x18, 0xff, 0xf3, 0x48, 0xc4, 0x3a, 0x2b, 0xda, 0x42, 0x6c, 0x55, 0x9d, 0xa0, 0x00, 0x2c, 0x03, 0x8a, 0x81, 0x50, 0x0e, 0xd6, 0x03, 0x18, 0x28, 0x20, 0x12, 0x02, 0x85, 0xc1, 0xb9, 0x62, 0xbc, 0x23, 0xe1, 0x3c, 0x90, 0xdd, 0xf0, 0xe2, 0x82, 0xd3, 0x03, 0xd0, 0x13, 0x68, 0x72, 0xe3, 0x84, 0x75, 0x19, 0x13, 0xc4, 0x6f, 0xe2, 0x93, 0x19, 0x61, 0x1e, 0x0c, 0xe1, 0x01, 0x17, 0x19, 0x89, 0x74, 0xa4, 0xa3, 0x63, 0xbf, 0xc7, 0xd9, 0x16, 0x19, 0x82, 0x50, 0x9d, 0x1c, 0x65, 0x15, 0xa2, 0xaa, 0x4b, 0xff, 0xcd, 0x89, 0xc3, 0x23, 0x52, 0x7c, 0xc4, 0xf9, 0x70, 0xc9, 0x15, 0x52, 0x5d, 0x1f, 0xff, 0xcf, 0x13, 0xe5, 0xd3, 0xe5, 0xc2, 0xf1, 0xa1, 0x7c, 0xd4, 0xdc, 0xb8, 0x6d, 0xff, 0xf4, 0x03, 0xa0, 0xe0, 0x10, 0x3c, 0x16, 0x02, 0x23, 0xff, 0xfe, 0x5d, 0x27, 0x00, 0x69, 0x38, 0x03, 0x49, 0xc1, 0xaa, 0x0d, 0xd7, 0x12, 0x5c, 0xc9, 0x0c, 0x8a, 0xca, 0x0c, 0xeb, 0x37, 0x52, 0xd4, 0xff, 0xf3, 0x48, 0xc4, 0x0d, 0x19, 0x88, 0xdd, 0xc8, 0x0d, 0xd8, 0x48, 0x00, 0x82, 0x82, 0xdc, 0xcc, 0x07, 0x34, 0x15, 0x24, 0x55, 0x58, 0xb2, 0x45, 0xda, 0x7c, 0xd8, 0x0a, 0xee, 0x6b, 0xcf, 0xd4, 0x75, 0x61, 0x54, 0x15, 0xd5, 0x74, 0x95, 0x30, 0x12, 0x58, 0x12, 0x04, 0x89, 0x9e, 0xa9, 0x08, 0xa6, 0x4a, 0x90, 0x92, 0xa1, 0x8d, 0x22, 0x45, 0x1a, 0x44, 0x89, 0xa9, 0x5c, 0x50, 0xe8, 0x94, 0x15, 0x3a, 0x0d, 0x38, 0x1a, 0x0e, 0x95, 0x0d, 0xac, 0x15, 0x70, 0x8b, 0x3d, 0xca, 0xf8, 0x76, 0xdf, 0xf6, 0xff, 0xf0, 0xd5, 0x6e, 0x50, 0x76, 0x9f, 0xf2, 0xdf, 0xff, 0xf2, 0xdf, 0xff, 0x0d, 0x4b, 0x1e, 0x11, 0x3e, 0x0a, 0xd5, 0x4c, 0x41, 0x4d, 0x45, 0x33, 0x2e, 0x31, 0x30, 0x31, 0x20, 0x28, 0x62, 0x65, 0x74, 0x61, 0x20, 0x33, 0x29, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55};
constexpr std::size_t mp3_frame_bytes = 144;

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

using audio_frame_source = std::function<std::vector<std::uint8_t>(std::int64_t)>;
using psm_rewrite = std::function<void(std::vector<std::array<std::uint8_t, 2>>&)>;

// 改写 PSM 时使用的特殊条目标记：
// invalid_psm_entry 写成 es_info_length 越界的坏条目；descriptor_overflow_entry 写成类型为 G.722、
// 描述符体越出 ES 信息范围的条目；trailing_byte_entry 只在映射末尾追加一个残缺字节。
// sid 为 0xfd 的条目写成带 3 字节扩展 ID 伪描述符的扩展流条目，扩展 ID 依次递增。
constexpr std::uint8_t invalid_psm_entry = 0xff;
constexpr std::uint8_t descriptor_overflow_entry = 0xfe;
constexpr std::uint8_t trailing_byte_entry = 0xfc;
constexpr std::uint8_t extension_stream_id = 0xfd;

// 按 rewrite 改写 PS 包中 PSM 的条目 (stream_type, stream_id)，版本号保持不变；返回是否改写。
bool rewrite_psm(std::vector<std::uint8_t>& packet, const psm_rewrite& rewrite)
{
    for (std::size_t index = 0; index + 4 <= packet.size(); ++index)
    {
        if (packet[index] != 0 || packet[index + 1] != 0 || packet[index + 2] != 1 || packet[index + 3] != 0xbc)
        {
            continue;
        }
        const auto length = static_cast<std::size_t>((packet[index + 4] << 8U) | packet[index + 5]);
        const auto info = static_cast<std::size_t>((packet[index + 8] << 8U) | packet[index + 9]);
        const auto map_begin = index + 12 + info;
        const auto map_end = index + 6 + length - 4;
        std::vector<std::array<std::uint8_t, 2>> entries;
        for (auto entry = map_begin; entry + 4 <= map_end;)
        {
            const auto entry_info = static_cast<std::size_t>((packet[entry + 2] << 8U) | packet[entry + 3]);
            require(entry_info == 0, "psm rewrite expects entries without descriptors");
            entries.push_back({packet[entry], packet[entry + 1]});
            entry += 4 + entry_info;
        }
        rewrite(entries);
        std::vector<std::uint8_t> map;
        std::uint8_t extension{};
        for (const auto& [type, sid] : entries)
        {
            if (type == trailing_byte_entry)
            {
                map.push_back(0x00);
            }
            else if (type == descriptor_overflow_entry)
            {
                map.insert(map.end(), {PSI_STREAM_AUDIO_G722, sid, 0x00, 0x02, 0x80, 0x03});
            }
            else if (sid == extension_stream_id)
            {
                map.insert(map.end(), {type, sid, 0x00, 0x03, 0x00, 0x01, static_cast<std::uint8_t>(0x80U | extension++)});
            }
            else
            {
                const std::uint8_t info_length = type == invalid_psm_entry ? 0xff : 0x00;
                map.insert(map.end(), {type, sid, info_length, info_length});
            }
        }
        std::vector<std::uint8_t> psm(packet.begin() + static_cast<std::ptrdiff_t>(index), packet.begin() + static_cast<std::ptrdiff_t>(map_begin));
        // program_stream_map_length 计入其后的全部字节：flags(2) + info_length(2) + info + map_length(2) + map + CRC(4)。
        const auto new_length = 10 + info + map.size();
        psm[4] = static_cast<std::uint8_t>(new_length >> 8U);
        psm[5] = static_cast<std::uint8_t>(new_length);
        psm[map_begin - index - 2] = static_cast<std::uint8_t>(map.size() >> 8U);
        psm[map_begin - index - 1] = static_cast<std::uint8_t>(map.size());
        psm.insert(psm.end(), map.begin(), map.end());
        // 与 ireader psm_write 相同的 CRC 计算与字节序。
        const auto crc = mpeg_crc32(0xffffffff, psm.data(), static_cast<std::uint32_t>(psm.size()));
        psm.insert(psm.end(), {static_cast<std::uint8_t>(crc), static_cast<std::uint8_t>(crc >> 8U), static_cast<std::uint8_t>(crc >> 16U),
                               static_cast<std::uint8_t>(crc >> 24U)});
        require(psm.size() == 6 + new_length && psm[6] == packet[index + 6], "psm rewrite length or version changed");
        packet.erase(packet.begin() + static_cast<std::ptrdiff_t>(index), packet.begin() + static_cast<std::ptrdiff_t>(index + 6 + length));
        packet.insert(packet.begin() + static_cast<std::ptrdiff_t>(index), psm.begin(), psm.end());
        return true;
    }
    return false;
}

// 生成 frames 帧"视频 IDR + 音频"的 PS 并打包为 RTP；从第 rewrite_from 帧起按 rewrite 改写 PSM。
std::vector<std::vector<std::uint8_t>> make_rtp(int audio_codec,
                                                const audio_frame_source& audio_frame,
                                                std::int64_t frames = 10,
                                                std::int64_t rewrite_from = -1,
                                                const psm_rewrite& rewrite = {})
{
    ps_writer ps;
    const ps_muxer_func_t ps_functions{&ps_writer::alloc, &ps_writer::free, &ps_writer::write};
    auto* muxer = ps_muxer_create(&ps_functions, &ps);
    const auto video = ps_muxer_add_stream(muxer, PSI_STREAM_H264, nullptr, 0);
    const auto audio = audio_codec != 0 ? ps_muxer_add_stream(muxer, audio_codec, nullptr, 0) : -1;
    std::size_t rewrite_packet = ps.packets.size();
    for (std::int64_t index = 0; index < frames; ++index)
    {
        if (index == rewrite_from)
        {
            rewrite_packet = ps.packets.size();
        }
        const auto pts = index * 3'600;
        require(ps_muxer_input(muxer, video, MPEG_FLAG_IDR_FRAME, pts, pts, h264_idr.data(), h264_idr.size()) == 0, "ps video mux failed");
        if (audio >= 0)
        {
            const auto data = audio_frame(index);
            require(ps_muxer_input(muxer, audio, 0, pts, pts, data.data(), data.size()) == 0, "ps audio mux failed");
        }
    }
    ps_muxer_destroy(muxer);
    if (rewrite)
    {
        std::size_t rewritten{};
        for (auto index = rewrite_packet; index < ps.packets.size(); ++index)
        {
            rewritten += rewrite_psm(ps.packets[index], rewrite) ? 1U : 0U;
        }
        require(rewritten != 0, "psm rewrite found no psm");
    }

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

std::vector<std::vector<std::uint8_t>> make_rtp(int audio_codec, std::size_t audio_bytes)
{
    return make_rtp(audio_codec, [audio_bytes](std::int64_t) { return std::vector<std::uint8_t>(audio_bytes, 0x55); });
}

// 送入全部 RTP 并检查登记的轨道数；遇到致命结果返回 false。
bool feed(const std::string& stream_name, const std::vector<std::vector<std::uint8_t>>& packets, std::size_t expected_tracks)
{
    worker_context worker;
    gb28181_rtp_receiver receiver(worker, stream_name, payload_type, ssrc);
    require(receiver.startup(), stream_name + ": receiver startup failed");
    for (const auto& packet : packets)
    {
        if (receiver.receive_rtp(packet) == gb28181_rtp_receive_result::fatal)
        {
            receiver.shutdown();
            return false;
        }
    }
    const auto stream = stream_registry::instance().find(stream_name);
    require(stream && stream->tracks().size() == expected_tracks, stream_name + ": unexpected tracks");
    receiver.shutdown();
    return true;
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
void device_selected_ssrc()
{
    worker_context worker;
    const std::string stream_name = "gb/test/device_ssrc";
    gb28181_rtp_receiver receiver(worker, stream_name, payload_type, ssrc + 1U);
    require(receiver.startup(), "device ssrc: receiver startup failed");
    const auto packets = make_rtp(PSI_STREAM_AUDIO_G711A, 320);
    for (const auto& packet : packets)
    {
        require(receiver.receive_rtp(packet) == gb28181_rtp_receive_result::ignored, "device ssrc: foreign ssrc accepted");
    }
    // 设备在 200 OK 的 y= 中返回自己的 SSRC 后改用该值过滤。
    receiver.set_expected_ssrc(ssrc);
    for (const auto& packet : packets)
    {
        require(receiver.receive_rtp(packet) == gb28181_rtp_receive_result::accepted, "device ssrc: updated ssrc rejected");
    }
    require(stream_registry::instance().find(stream_name) != nullptr, "device ssrc: stream not registered");
    receiver.shutdown();
    std::cout << "device_selected_ssrc: PASS\n";
}
void empty_payload_is_not_media()
{
    worker_context worker;
    gb28181_rtp_receiver receiver(worker, "gb/test/empty_payload", payload_type, ssrc);
    require(receiver.startup(), "empty payload: receiver startup failed");
    for (const auto& packet : make_rtp(PSI_STREAM_AUDIO_G711A, 320))
    {
        require(receiver.receive_rtp(packet) != gb28181_rtp_receive_result::fatal, "empty payload: stream rejected");
    }
    // 只有 RTP 头、没有 PS 载荷的包不是媒体。
    for (std::uint16_t sequence = 1000; sequence < 1100; ++sequence)
    {
        std::array<std::uint8_t, 12> header{0x80, payload_type, static_cast<std::uint8_t>(sequence >> 8U), static_cast<std::uint8_t>(sequence),
                                            0, 0, 0, 0,
                                            static_cast<std::uint8_t>(ssrc >> 24U), static_cast<std::uint8_t>(ssrc >> 16U),
                                            static_cast<std::uint8_t>(ssrc >> 8U), static_cast<std::uint8_t>(ssrc)};
        // ignored 才能保证 UDP 会话不会把对端锁定到发送空包的端点。
        require(receiver.receive_rtp(header) == gb28181_rtp_receive_result::ignored, "empty payload: header-only rtp accepted");
    }
    receiver.shutdown();
    std::cout << "empty_payload_is_not_media: PASS\n";
}
// 先发送带数据的视频与 AAC 使流就绪，随后只发送只有 ADTS 头的 AAC PES；返回 RTP 包及第二阶段起点。
void real_mp3_audio()
{
    // 有效 MP3 能被 ireader 映射并解析，到达上层后按无音频忽略。对照用同样带音频 PES、
    // 但音频在 ireader 中被丢弃的 G.722 输入，两者的视频帧刷新时机相同，发布帧数必须一致。
    const auto mp3 = feed("gb/test/mp3_real",
                          make_rtp(PSI_STREAM_MP3,
                                   [](std::int64_t index)
                                   {
                                       const auto offset = static_cast<std::size_t>(index % 8) * mp3_frame_bytes;
                                       return std::vector<std::uint8_t>(mp3_frames.begin() + static_cast<std::ptrdiff_t>(offset),
                                                                        mp3_frames.begin() + static_cast<std::ptrdiff_t>(offset + mp3_frame_bytes));
                                   }),
                          1);
    const auto dropped = feed("gb/test/mp3_control", make_rtp(PSI_STREAM_AUDIO_G722, mp3_frame_bytes), 1);
    require(mp3 && dropped, "real mp3: stream rejected");
    std::cout << "real_mp3_audio: PASS\n";
}

void psm_topology_changes()
{
    const auto g711 = [](std::int64_t) { return std::vector<std::uint8_t>(320, 0x55); };
    // 同版本、同条目数的 PSM 把同一 SID 从 G.711A 改为 G.722：拓扑变化必须结束当前代。
    const auto changed = feed("gb/test/psm_codec_change",
                              make_rtp(PSI_STREAM_AUDIO_G711A, g711, 20, 10,
                                       [](std::vector<std::array<std::uint8_t, 2>>& entries)
                                       {
                                           for (auto& entry : entries)
                                           {
                                               if (entry[0] == PSI_STREAM_AUDIO_G711A)
                                               {
                                                   entry[0] = PSI_STREAM_AUDIO_G722;
                                               }
                                           }
                                       }),
                              2);
    require(!changed, "psm codec change on same version not rejected");
    // PSM 删除音频条目：累积历史表仍保留旧条目，活动集合比较必须识别删除。
    const auto removed = feed("gb/test/psm_audio_removed",
                              make_rtp(PSI_STREAM_AUDIO_G711A, g711, 20, 10,
                                       [](std::vector<std::array<std::uint8_t, 2>>& entries)
                                       { std::erase_if(entries, [](const auto& entry) { return entry[0] == PSI_STREAM_AUDIO_G711A; }); }),
                              2);
    require(!removed, "psm audio entry removal not rejected");
    // PSM 删除全部条目：空活动集合也必须通知上层并结束当前代。
    const auto emptied = feed("gb/test/psm_emptied",
                              make_rtp(PSI_STREAM_AUDIO_G711A, g711, 20, 10, [](std::vector<std::array<std::uint8_t, 2>>& entries) { entries.clear(); }),
                              2);
    require(!emptied, "empty psm not rejected");
    // 先把 G.711A 改为 G.722 再接一个坏条目：解析失败不能提交任何改动，已有音视频照常发布。
    const auto unchanged = feed("gb/test/psm_unchanged", make_rtp(PSI_STREAM_AUDIO_G711A, g711, 20), 2);
    const auto failed = feed("gb/test/psm_failed_parse",
                             make_rtp(PSI_STREAM_AUDIO_G711A, g711, 20, 10,
                                      [](std::vector<std::array<std::uint8_t, 2>>& entries)
                                      {
                                          for (auto& entry : entries)
                                          {
                                              if (entry[0] == PSI_STREAM_AUDIO_G711A)
                                              {
                                                  entry[0] = PSI_STREAM_AUDIO_G722;
                                              }
                                          }
                                          entries.push_back({invalid_psm_entry, 0xc1});
                                      }),
                             2);
    require(unchanged && failed, "failed psm parse committed partial topology");
    // 描述符越出 ES 信息范围、映射末尾残缺：都必须判为解析失败，不能提交 C0 的编码变化。
    const auto to_g722 = [](std::vector<std::array<std::uint8_t, 2>>& entries)
    {
        for (auto& entry : entries)
        {
            if (entry[0] == PSI_STREAM_AUDIO_G711A)
            {
                entry[0] = PSI_STREAM_AUDIO_G722;
            }
        }
    };
    const auto overflow = feed("gb/test/psm_descriptor_overflow",
                               make_rtp(PSI_STREAM_AUDIO_G711A, g711, 20, 10,
                                        [](std::vector<std::array<std::uint8_t, 2>>& entries)
                                        {
                                            for (auto& entry : entries)
                                            {
                                                if (entry[0] == PSI_STREAM_AUDIO_G711A)
                                                {
                                                    entry[0] = descriptor_overflow_entry;
                                                }
                                            }
                                        }),
                               2);
    require(overflow, "psm descriptor overflow committed topology");
    const auto trailing = feed("gb/test/psm_trailing_byte",
                               make_rtp(PSI_STREAM_AUDIO_G711A, g711, 20, 10,
                                        [&to_g722](std::vector<std::array<std::uint8_t, 2>>& entries)
                                        {
                                            to_g722(entries);
                                            entries.push_back({trailing_byte_entry, 0x00});
                                        }),
                               2);
    require(trailing, "psm trailing byte committed topology");
    std::cout << "psm_topology_changes: PASS\n";
}

void psm_history_capacity()
{
    const auto g722 = [](std::int64_t) { return std::vector<std::uint8_t>(160, 0x55); };
    const auto control = feed("gb/test/psm_capacity_control", make_rtp(PSI_STREAM_AUDIO_G722, g722, 40), 1);
    // 不支持的音频不参与拓扑比较，反复更换其 SID 会累积历史表直到满载；
    // 超出容量的 PSM 整体失败且不影响已有输入，满载后同样的 SID 也必须仍能正确解析。
    std::uint8_t next_sid = 0xc1;
    const auto churn = feed("gb/test/psm_capacity_churn",
                            make_rtp(PSI_STREAM_AUDIO_G722, g722, 40, 1,
                                     [&next_sid](std::vector<std::array<std::uint8_t, 2>>& entries)
                                     {
                                         for (auto& entry : entries)
                                         {
                                             if (entry[0] == PSI_STREAM_AUDIO_G722)
                                             {
                                                 entry[1] = next_sid;
                                             }
                                         }
                                         next_sid = next_sid == 0xdf ? 0xc1 : static_cast<std::uint8_t>(next_sid + 1U);
                                     }),
                            1);
    require(control && churn, "psm history capacity churn changed topology");

    // 历史表已有 E0/C0/C1 时，PSM 含 14 个同为 0xfd 的扩展流条目只占一个新槽位；
    // 同时把 C0 改为受支持的 G.711A，这一拓扑变化必须被提交并结束当前代。
    int call = 0;
    const auto extensions = feed("gb/test/psm_extension_streams",
                                 make_rtp(PSI_STREAM_AUDIO_G722, g722, 20, 1,
                                          [&call](std::vector<std::array<std::uint8_t, 2>>& entries)
                                          {
                                              if (call++ == 0)
                                              {
                                                  entries.push_back({PSI_STREAM_AUDIO_G722, 0xc1});
                                                  return;
                                              }
                                              for (auto& entry : entries)
                                              {
                                                  if (entry[0] == PSI_STREAM_AUDIO_G722)
                                                  {
                                                      entry[0] = PSI_STREAM_AUDIO_G711A;
                                                  }
                                              }
                                              for (int index = 0; index < 14; ++index)
                                              {
                                                  entries.push_back({PSI_STREAM_AUDIO_G722, extension_stream_id});
                                              }
                                          }),
                                 1);
    require(!extensions, "psm with repeated extension stream ids rejected; topology change missed");
    std::cout << "psm_history_capacity: PASS\n";
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
        device_selected_ssrc();
        empty_payload_is_not_media();
        real_mp3_audio();
        psm_topology_changes();
        psm_history_capacity();
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
