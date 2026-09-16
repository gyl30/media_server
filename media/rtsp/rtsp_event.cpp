#include <string>

#include "media/rtsp/rtsp_event.h"

namespace media_server::rtsp_event
{

runtime_event publisher_starting(std::string_view stream_id, std::string_view stream_name)
{
    return {
        .kind = runtime_kind::publisher,
        .stream_id = std::string{stream_id},
        .stream_name = std::string{stream_name},
        .protocol = runtime_protocol::rtsp,
        .state = runtime_state::starting,
        .stage = "announce",
    };
}

runtime_event publisher_streaming(std::string_view stream_id, std::string_view stream_name)
{
    return {
        .kind = runtime_kind::publisher,
        .stream_id = std::string{stream_id},
        .stream_name = std::string{stream_name},
        .protocol = runtime_protocol::rtsp,
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
        .protocol = runtime_protocol::rtsp,
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

runtime_event source_starting(std::string_view stream_id, std::string_view stream_name, std::string_view source_id)
{
    return {
        .kind = runtime_kind::source,
        .stream_id = std::string{stream_id},
        .stream_name = std::string{stream_name},
        .source_id = std::string{source_id},
        .protocol = runtime_protocol::rtsp,
        .state = runtime_state::starting,
        .stage = "resolving",
    };
}

runtime_event source_streaming(std::string_view stream_id, std::string_view stream_name, std::string_view source_id)
{
    return {
        .kind = runtime_kind::source,
        .stream_id = std::string{stream_id},
        .stream_name = std::string{stream_name},
        .source_id = std::string{source_id},
        .protocol = runtime_protocol::rtsp,
        .state = runtime_state::streaming,
        .stage = "streaming",
    };
}

runtime_event source_stopped(
    std::string_view stream_id, std::string_view stream_name, std::string_view source_id, runtime_end_reason reason, std::string_view error)
{
    runtime_event event{
        .kind = runtime_kind::source,
        .stream_id = std::string{stream_id},
        .stream_name = std::string{stream_name},
        .source_id = std::string{source_id},
        .protocol = runtime_protocol::rtsp,
        .state = runtime_state::stopped,
        .end_reason = reason,
    };
    if (!error.empty())
    {
        event.error = std::string{error};
    }
    return event;
}

}    // namespace media_server::rtsp_event
