#ifndef MEDIA_RTSP_RTSP_URI_H
#define MEDIA_RTSP_RTSP_URI_H

#include <optional>
#include <string>
#include <string_view>

#include "media/core/media_types.h"

namespace media_server
{

[[nodiscard]] std::string rtsp_stream_name_from_uri(std::string_view uri);
[[nodiscard]] std::optional<track_id> rtsp_track_id_from_uri(std::string_view uri);

}    // namespace media_server

#endif
