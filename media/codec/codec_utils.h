#ifndef MEDIA_CODEC_CODEC_UTILS_H
#define MEDIA_CODEC_CODEC_UTILS_H

#include <span>
#include <vector>
#include <cstdint>
#include <optional>

#include "media/core/media_types.h"

struct avstream_t;

namespace media_server
{

struct aac_config
{
    std::uint32_t sample_rate{};
    std::uint16_t channel_count{};
};

[[nodiscard]] std::vector<std::uint8_t> h264_avcc_to_annex_b(std::span<const std::uint8_t> avcc);

[[nodiscard]] std::vector<std::uint8_t> h264_annex_b_to_avcc(std::span<const std::uint8_t> annex_b);

[[nodiscard]] std::vector<std::uint8_t> h265_hvcc_to_annex_b(std::span<const std::uint8_t> hvcc);

[[nodiscard]] std::vector<std::uint8_t> h265_annex_b_to_hvcc(std::span<const std::uint8_t> annex_b);

[[nodiscard]] std::optional<aac_config> parse_aac_asc(std::span<const std::uint8_t> asc);

[[nodiscard]] std::optional<aac_config> parse_aac_adts(std::span<const std::uint8_t> adts);

[[nodiscard]] std::vector<std::uint8_t> make_adts_frame(std::span<const std::uint8_t> asc, std::span<const std::uint8_t> raw_aac);

[[nodiscard]] std::optional<media_track> media_track_from_avstream_config(const avstream_t& input, track_id video_track_id, track_id audio_track_id);

[[nodiscard]] std::int64_t milliseconds_to_ns(std::int64_t value) noexcept;
[[nodiscard]] std::int64_t ns_to_milliseconds(std::int64_t value) noexcept;
[[nodiscard]] std::uint32_t ns_to_flv_milliseconds(std::int64_t value) noexcept;
[[nodiscard]] std::int64_t ns_to_90khz(std::int64_t value) noexcept;

}    // namespace media_server

#endif
