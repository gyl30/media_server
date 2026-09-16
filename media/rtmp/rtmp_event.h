#ifndef MEDIA_RTMP_RTMP_EVENT_H
#define MEDIA_RTMP_RTMP_EVENT_H

#include <string_view>

#include "media/core/runtime_event.h"

namespace media_server::rtmp_event
{

[[nodiscard]] runtime_event publisher_starting(std::string_view stream_id, std::string_view stream_name);
[[nodiscard]] runtime_event publisher_streaming(std::string_view stream_id, std::string_view stream_name);
[[nodiscard]] runtime_event publisher_stopped(
    std::string_view stream_id, std::string_view stream_name, runtime_end_reason reason, std::string_view stage = {}, std::string_view error = {});

}    // namespace media_server::rtmp_event

#endif
