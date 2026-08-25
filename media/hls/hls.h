#ifndef MEDIA_HLS_H
#define MEDIA_HLS_H

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "config.h"

namespace media_server::hls
{

[[nodiscard]] std::optional<std::string> playlist(std::string_view stream_name, const config& config);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> init_segment(std::string_view stream_name, const config& config);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> segment(std::string_view stream_name, std::uint64_t sequence, const config& config);
[[nodiscard]] std::optional<std::size_t> segment_count(std::string_view stream_name, const config& config);

void shutdown();

}    // namespace media_server::hls

#endif
