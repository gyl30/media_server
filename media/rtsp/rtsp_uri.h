#ifndef MEDIA_RTSP_RTSP_URI_H
#define MEDIA_RTSP_RTSP_URI_H

#include <string>
#include <string_view>

namespace media_server
{

[[nodiscard]] std::string rtsp_path_from_uri(std::string_view uri);

}    // namespace media_server

#endif
