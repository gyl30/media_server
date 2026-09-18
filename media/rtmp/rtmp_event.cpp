#include "media/rtmp/rtmp_event.h"

namespace media_server::rtmp_event
{

runtime_event make_publisher(
    event_state state, std::string_view stream_id, std::string_view stream_name, std::string_view stage, std::string_view error)
{
    return make_event(event_kind::publisher, event_protocol::rtmp, state, stream_id, stream_name, {}, stage, error);
}

}    // namespace media_server::rtmp_event
