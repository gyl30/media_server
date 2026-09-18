#include "media/gb28181/gb28181_event.h"

namespace media_server::gb28181_event
{

runtime_event make_source(event_state state, std::string_view stream_id, std::string_view stream_name, std::string_view stage, std::string_view error)
{
    return make_event(event_kind::source, event_protocol::gb28181, state, stream_id, stream_name, {}, stage, error);
}

runtime_event make_output(event_state state, std::string_view stream_id, std::string_view stream_name, std::string_view stage, std::string_view error)
{
    return make_event(event_kind::output, event_protocol::gb28181, state, stream_id, stream_name, {}, stage, error);
}

}    // namespace media_server::gb28181_event
