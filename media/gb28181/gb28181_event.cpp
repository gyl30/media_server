#include <string>

#include "media/gb28181/gb28181_event.h"

namespace media_server::gb28181_event
{

namespace
{

runtime_event stopped(runtime_kind kind, std::string_view stream_id, std::string_view stream_name, runtime_end_reason reason, std::string_view error)
{
    runtime_event event{
        .kind = kind,
        .stream_id = std::string{stream_id},
        .stream_name = std::string{stream_name},
        .protocol = runtime_protocol::gb28181,
        .state = runtime_state::stopped,
        .end_reason = reason,
    };
    if (!error.empty())
    {
        event.error = std::string{error};
    }
    return event;
}

}    // namespace

runtime_event source_starting(std::string_view stream_id, std::string_view stream_name, std::string_view stage)
{
    return {
        .kind = runtime_kind::source,
        .stream_id = std::string{stream_id},
        .stream_name = std::string{stream_name},
        .protocol = runtime_protocol::gb28181,
        .state = runtime_state::starting,
        .stage = std::string{stage},
    };
}

runtime_event source_streaming(std::string_view stream_id, std::string_view stream_name)
{
    return {
        .kind = runtime_kind::source,
        .stream_id = std::string{stream_id},
        .stream_name = std::string{stream_name},
        .protocol = runtime_protocol::gb28181,
        .state = runtime_state::streaming,
        .stage = "streaming",
    };
}

runtime_event source_stopped(std::string_view stream_id, std::string_view stream_name, runtime_end_reason reason, std::string_view error)
{
    return stopped(runtime_kind::source, stream_id, stream_name, reason, error);
}

runtime_event output_starting(std::string_view stream_id, std::string_view stream_name, std::string_view stage)
{
    runtime_event event{
        .kind = runtime_kind::output,
        .stream_id = std::string{stream_id},
        .stream_name = std::string{stream_name},
        .protocol = runtime_protocol::gb28181,
        .state = runtime_state::starting,
    };
    if (!stage.empty())
    {
        event.stage = std::string{stage};
    }
    return event;
}

runtime_event output_streaming(std::string_view stream_id, std::string_view stream_name)
{
    return {
        .kind = runtime_kind::output,
        .stream_id = std::string{stream_id},
        .stream_name = std::string{stream_name},
        .protocol = runtime_protocol::gb28181,
        .state = runtime_state::streaming,
        .stage = "streaming",
    };
}

runtime_event output_stopped(std::string_view stream_id, std::string_view stream_name, runtime_end_reason reason, std::string_view error)
{
    return stopped(runtime_kind::output, stream_id, stream_name, reason, error);
}

}    // namespace media_server::gb28181_event
