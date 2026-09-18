#ifndef MEDIA_RTSP_RTSP_EVENT_H
#define MEDIA_RTSP_RTSP_EVENT_H

#include <string_view>

#include "media/core/runtime_event.h"

namespace media_server::rtsp_event
{

[[nodiscard]] runtime_event make_publisher(
    event_state state, std::string_view stream_id, std::string_view stream_name, std::string_view stage = {}, std::string_view error = {});

[[nodiscard]] runtime_event make_source(event_state state,
                                        std::string_view stream_id,
                                        std::string_view stream_name,
                                        std::string_view source_id,
                                        std::string_view stage = {},
                                        std::string_view error = {});

}    // namespace media_server::rtsp_event

#endif
