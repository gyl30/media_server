#ifndef MEDIA_RTMP_RTMP_EVENT_H
#define MEDIA_RTMP_RTMP_EVENT_H

#include <string_view>

#include "media/core/runtime_event.h"

namespace media_server::rtmp_event
{

void report_publisher(
    event_state state, std::string_view stream_id, std::string_view stream_name, std::string_view stage = {}, std::string_view error = {});

}

#endif
