#ifndef MEDIA_GB28181_GB28181_EVENT_H
#define MEDIA_GB28181_GB28181_EVENT_H

#include <string_view>

#include "media/core/runtime_event.h"

namespace media_server::gb28181_event
{

void report_source(
    event_state state, std::string_view stream_id, std::string_view stream_name, std::string_view stage = {}, std::string_view error = {});

void report_output(
    event_state state, std::string_view stream_id, std::string_view stream_name, std::string_view stage = {}, std::string_view error = {});

}    // namespace media_server::gb28181_event

#endif
