#ifndef MEDIA_RTSP_RTSP_URI_H
#define MEDIA_RTSP_RTSP_URI_H

#include <string>
#include <optional>
#include <string_view>

namespace media_server
{

struct rtsp_publish_target
{
    std::string stream_id;
    std::string stream_name;
};

[[nodiscard]] std::string rtsp_path_from_uri(std::string_view uri);
[[nodiscard]] std::optional<rtsp_publish_target> parse_rtsp_publish_target(std::string_view uri);

}    // namespace media_server

#endif
