#include "media/rtsp/rtsp_event.h"

#include "media/http/signaling_client.h"

namespace media_server::rtsp_event
{

void report_publisher(event_state state, std::string_view stream_id, std::string_view stream_name, std::string_view stage, std::string_view error)
{
    signaling_client::instance().report(make_event(event_kind::publisher, event_protocol::rtsp, state, stream_id, stream_name, {}, stage, error));
}

void report_source(event_state state,
                   std::string_view stream_id,
                   std::string_view stream_name,
                   std::string_view source_id,
                   std::string_view stage,
                   std::string_view error)
{
    signaling_client::instance().report(make_event(event_kind::source, event_protocol::rtsp, state, stream_id, stream_name, source_id, stage, error));
}

void report_output(event_state state, std::string_view stream_id, std::string_view stream_name, std::string_view stage, std::string_view error)
{
    signaling_client::instance().report(make_event(event_kind::output, event_protocol::rtsp, state, stream_id, stream_name, {}, stage, error));
}

}    // namespace media_server::rtsp_event
