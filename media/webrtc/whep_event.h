#ifndef MEDIA_WEBRTC_WHEP_EVENT_H
#define MEDIA_WEBRTC_WHEP_EVENT_H

#include <string_view>

#include "media/core/runtime_event.h"

namespace media_server::whep_event
{

[[nodiscard]] runtime_event output_starting(std::string_view stream_id, std::string_view stream_name);
[[nodiscard]] runtime_event output_streaming(std::string_view stream_id, std::string_view stream_name);
[[nodiscard]] runtime_event output_stopped(std::string_view stream_id,
                                           std::string_view stream_name,
                                           runtime_end_reason reason,
                                           std::string_view error = {});

}    // namespace media_server::whep_event

#endif
