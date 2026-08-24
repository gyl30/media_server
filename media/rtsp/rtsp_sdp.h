#ifndef MEDIA_RTSP_RTSP_SDP_H
#define MEDIA_RTSP_RTSP_SDP_H

#include <vector>
#include <cstdint>
#include <string_view>

namespace media_server
{

[[nodiscard]] bool rtsp_sdp_iequals(const char* value, std::string_view expected);
[[nodiscard]] bool rtsp_sdp_append_parameter_sets(std::vector<std::uint8_t>& config, std::string_view encoded);

}    // namespace media_server

#endif
