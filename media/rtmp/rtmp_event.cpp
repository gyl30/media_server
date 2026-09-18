#include "media/rtmp/rtmp_event.h"
#include "media/http/signaling_client.h"

namespace media_server::rtmp_event
{

void report_publisher(event_state state, std::string_view stream_id, std::string_view stream_name, std::string_view stage, std::string_view error)
{
    signaling_client::instance().report(make_event(event_kind::publisher, event_protocol::rtmp, state, stream_id, stream_name, {}, stage, error));
}

void report_output(event_state state, std::string_view stream_id, std::string_view stream_name, std::string_view stage, std::string_view error)
{
    signaling_client::instance().report(make_event(event_kind::output, event_protocol::rtmp, state, stream_id, stream_name, {}, stage, error));
}

}    // namespace media_server::rtmp_event
