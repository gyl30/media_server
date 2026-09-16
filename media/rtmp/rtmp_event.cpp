#include <string>

#include "media/rtmp/rtmp_event.h"

namespace media_server::rtmp_event
{

runtime_event publisher_starting(std::string_view stream_id, std::string_view stream_name)
{
    return {
        .kind = runtime_kind::publisher,
        .stream_id = std::string{stream_id},
        .stream_name = std::string{stream_name},
        .protocol = runtime_protocol::rtmp,
        .state = runtime_state::starting,
        .stage = "publish",
    };
}

runtime_event publisher_streaming(std::string_view stream_id, std::string_view stream_name)
{
    return {
        .kind = runtime_kind::publisher,
        .stream_id = std::string{stream_id},
        .stream_name = std::string{stream_name},
        .protocol = runtime_protocol::rtmp,
        .state = runtime_state::streaming,
        .stage = "streaming",
    };
}

runtime_event publisher_stopped(
    std::string_view stream_id, std::string_view stream_name, runtime_end_reason reason, std::string_view stage, std::string_view error)
{
    runtime_event event{
        .kind = runtime_kind::publisher,
        .stream_id = std::string{stream_id},
        .stream_name = std::string{stream_name},
        .protocol = runtime_protocol::rtmp,
        .state = runtime_state::stopped,
        .end_reason = reason,
    };
    if (!stage.empty())
    {
        event.stage = std::string{stage};
    }
    if (!error.empty())
    {
        event.error = std::string{error};
    }
    return event;
}

}    // namespace media_server::rtmp_event
