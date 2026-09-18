#include "media/rtsp/rtsp_event.h"

namespace media_server::rtsp_event
{

runtime_event make_publisher(
    event_state state, std::string_view stream_id, std::string_view stream_name, std::string_view stage, std::string_view error)
{
    return make_event(event_kind::publisher, event_protocol::rtsp, state, stream_id, stream_name, {}, stage, error);
}

runtime_event make_source(event_state state,
                          std::string_view stream_id,
                          std::string_view stream_name,
                          std::string_view source_id,
                          std::string_view stage,
                          std::string_view error)
{
    return make_event(event_kind::source, event_protocol::rtsp, state, stream_id, stream_name, source_id, stage, error);
}

}    // namespace media_server::rtsp_event
