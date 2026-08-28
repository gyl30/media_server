#ifndef MEDIA_RTSP_RTSP_SDP_H
#define MEDIA_RTSP_RTSP_SDP_H

#include <vector>
#include <cstdint>
#include <optional>
#include <string_view>

#include "media/core/media_types.h"

namespace media_server
{

[[nodiscard]] bool rtsp_sdp_iequals(const char* value, std::string_view expected);
[[nodiscard]] std::optional<media_track> rtsp_sdp_track_from_format(
    const char* media, int payload_type, int rate, const char* encoding, const char* fmtp, track_id id);
[[nodiscard]] std::optional<std::uint16_t> rtsp_sdp_opus_channel_count(const char* fmtp);
[[nodiscard]] bool rtsp_sdp_append_parameter_sets(std::vector<std::uint8_t>& config, std::string_view encoded);

}    // namespace media_server

#endif
