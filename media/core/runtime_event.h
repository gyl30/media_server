#ifndef MEDIA_CORE_RUNTIME_EVENT_H
#define MEDIA_CORE_RUNTIME_EVENT_H

#include <string>
#include <optional>
#include <string_view>

namespace media_server
{

enum class event_kind
{
    source,
    publisher,
    output,
};

enum class event_protocol
{
    rtmp,
    rtsp,
    http_flv,
    hls,
    gb28181,
    whep,
};

enum class event_state
{
    starting,
    streaming,
    stop_requested,
    remote_closed,
    timeout,
    protocol_error,
    runtime_error,
    stopped,
};

struct runtime_event
{
    event_kind kind{};
    std::string stream_id{};
    std::string stream_name{};
    std::optional<std::string> source_id{};
    event_protocol protocol{};
    event_state state{};
    std::optional<std::string> stage{};
    std::optional<std::string> error{};
};

[[nodiscard]] inline runtime_event make_event(event_kind kind,
                                              event_protocol protocol,
                                              event_state state,
                                              std::string_view stream_id,
                                              std::string_view stream_name,
                                              std::string_view source_id = {},
                                              std::string_view stage = {},
                                              std::string_view error = {})
{
    runtime_event event{
        .kind = kind,
        .stream_id = std::string{stream_id},
        .stream_name = std::string{stream_name},
        .protocol = protocol,
        .state = state,
    };
    if (!source_id.empty())
    {
        event.source_id = std::string{source_id};
    }
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

[[nodiscard]] constexpr std::string_view to_string(event_kind value) noexcept
{
    switch (value)
    {
        case event_kind::source:
            return "source";
        case event_kind::publisher:
            return "publisher";
        case event_kind::output:
            return "output";
    }
    return {};
}

[[nodiscard]] constexpr std::string_view to_string(event_protocol value) noexcept
{
    switch (value)
    {
        case event_protocol::rtmp:
            return "rtmp";
        case event_protocol::rtsp:
            return "rtsp";
        case event_protocol::http_flv:
            return "http-flv";
        case event_protocol::hls:
            return "hls";
        case event_protocol::gb28181:
            return "gb28181";
        case event_protocol::whep:
            return "whep";
    }
    return {};
}

[[nodiscard]] constexpr std::string_view to_string(event_state value) noexcept
{
    switch (value)
    {
        case event_state::starting:
            return "starting";
        case event_state::streaming:
            return "streaming";
        case event_state::stop_requested:
            return "stop_requested";
        case event_state::remote_closed:
            return "remote_closed";
        case event_state::timeout:
            return "timeout";
        case event_state::protocol_error:
            return "protocol_error";
        case event_state::runtime_error:
            return "runtime_error";
        case event_state::stopped:
            return "stopped";
    }
    return {};
}

}    // namespace media_server

#endif
