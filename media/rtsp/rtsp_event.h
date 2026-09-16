#ifndef MEDIA_RTSP_RTSP_EVENT_H
#define MEDIA_RTSP_RTSP_EVENT_H

#include <string_view>

#include "media/core/runtime_event.h"

namespace media_server::rtsp_event
{

[[nodiscard]] runtime_event publisher_starting(std::string_view stream_id, std::string_view stream_name);
[[nodiscard]] runtime_event publisher_streaming(std::string_view stream_id, std::string_view stream_name);
[[nodiscard]] runtime_event publisher_stopped(
    std::string_view stream_id, std::string_view stream_name, runtime_end_reason reason, std::string_view stage = {}, std::string_view error = {});

[[nodiscard]] runtime_event source_starting(std::string_view stream_id, std::string_view stream_name, std::string_view source_id);
[[nodiscard]] runtime_event source_streaming(std::string_view stream_id, std::string_view stream_name, std::string_view source_id);
[[nodiscard]] runtime_event source_stopped(
    std::string_view stream_id, std::string_view stream_name, std::string_view source_id, runtime_end_reason reason, std::string_view error = {});

}    // namespace media_server::rtsp_event

#endif
