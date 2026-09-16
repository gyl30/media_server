#include <string>

#include "media/webrtc/whep_event.h"

namespace media_server::whep_event
{

runtime_event output_starting(std::string_view stream_id, std::string_view stream_name)
{
    return {
        .kind = runtime_kind::output,
        .stream_id = std::string{stream_id},
        .stream_name = std::string{stream_name},
        .protocol = runtime_protocol::whep,
        .state = runtime_state::starting,
        .stage = "ice",
    };
}

runtime_event output_streaming(std::string_view stream_id, std::string_view stream_name)
{
    return {
        .kind = runtime_kind::output,
        .stream_id = std::string{stream_id},
        .stream_name = std::string{stream_name},
        .protocol = runtime_protocol::whep,
        .state = runtime_state::streaming,
        .stage = "streaming",
    };
}

runtime_event output_stopped(std::string_view stream_id, std::string_view stream_name, runtime_end_reason reason, std::string_view error)
{
    runtime_event event{
        .kind = runtime_kind::output,
        .stream_id = std::string{stream_id},
        .stream_name = std::string{stream_name},
        .protocol = runtime_protocol::whep,
        .state = runtime_state::stopped,
        .end_reason = reason,
    };
    if (!error.empty())
    {
        event.error = std::string{error};
    }
    return event;
}

}    // namespace media_server::whep_event
