#ifndef MEDIA_WEBRTC_WHEP_EVENT_H
#define MEDIA_WEBRTC_WHEP_EVENT_H

#include <string_view>

#include "media/core/runtime_event.h"

namespace media_server::whep_event
{

[[nodiscard]] runtime_event make_output(
    event_state state, std::string_view stream_id, std::string_view stream_name, std::string_view stage = {}, std::string_view error = {});

}

#endif
