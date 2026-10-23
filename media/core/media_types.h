#ifndef MEDIA_CORE_MEDIA_TYPES_H
#define MEDIA_CORE_MEDIA_TYPES_H

#include <memory>
#include <vector>
#include <cstdint>
#include <string_view>

namespace media_server
{

using track_id = std::uint32_t;
using byte_buffer = std::shared_ptr<const std::vector<std::uint8_t>>;

class length_prefixed_payload;

enum class media_kind
{
    audio,
    video,
};

enum class codec_id
{
    h264,
    h265,
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

    std::vector<std::uint8_t> codec_config;
};

struct media_frame
{
    track_id track{};
    std::int64_t dts_ns{};
    std::int64_t pts_ns{};
    bool key_frame{};

    byte_buffer payload;
    std::shared_ptr<length_prefixed_payload> length_prefixed{};
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
