#ifndef MEDIA_CORE_MEDIA_TYPES_H
#define MEDIA_CORE_MEDIA_TYPES_H

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace media_server
{

using track_id = std::uint32_t;
using byte_buffer = std::shared_ptr<const std::vector<std::uint8_t>>;

enum class media_kind
{
    audio,
    video,
};

enum class codec_id
{
    h264,
    h265,
    av1,
    aac,
    opus,
    g711a,
    g711u,
};

struct media_track
{
    track_id id{};
    media_kind kind{};
    codec_id codec{};
    std::uint32_t clock_rate{};
    std::uint16_t channel_count{};

    // H.264 为 Annex-B SPS/PPS；H.265 为 Annex-B VPS/SPS/PPS；AAC 为 AudioSpecificConfig；无需额外配置的 codec 可为空。
    std::vector<std::uint8_t> codec_config;

    // 由 media_stream 维护，同一 track 实际配置变化时递增。
    std::uint64_t config_version{};
};

struct media_frame
{
    track_id track{};
    std::int64_t dts_ns{};
    std::int64_t pts_ns{};
    bool key_frame{};

    // H.264/H.265 为 Annex-B access unit；AAC 为完整 ADTS frame。
    byte_buffer payload;
};

[[nodiscard]] constexpr std::string_view to_string(media_kind kind) noexcept
{
    switch (kind)
    {
        case media_kind::audio:
            return "audio";
        case media_kind::video:
            return "video";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::string_view to_string(codec_id codec) noexcept
{
    switch (codec)
    {
        case codec_id::h264:
            return "h264";
        case codec_id::h265:
            return "h265";
        case codec_id::av1:
            return "av1";
        case codec_id::aac:
            return "aac";
        case codec_id::opus:
            return "opus";
        case codec_id::g711a:
            return "g711a";
        case codec_id::g711u:
            return "g711u";
    }
    return "unknown";
}

}    // namespace media_server

#endif
