#ifndef MEDIA_CORE_RUNTIME_EVENT_H
#define MEDIA_CORE_RUNTIME_EVENT_H

#include <memory>
#include <string>
#include <utility>
#include <optional>
#include <functional>
#include <string_view>

namespace media_server
{

enum class runtime_kind
{
    source,
    publisher,
    output,
};

enum class runtime_protocol
{
    rtmp,
    rtsp,
    gb28181,
    whep,
};

enum class runtime_state
{
    starting,
    streaming,
    stopped,
};

enum class runtime_end_reason
{
    requested,
    remote,
    timeout,
    protocol_error,
    runtime_error,
    server_shutdown,
};

struct runtime_event
{
    runtime_kind kind{};
    std::string server_id{};
    std::string instance_id{};
    std::string stream_id{};
    std::string stream_name{};
    std::optional<std::string> source_id{};
    runtime_protocol protocol{};
    runtime_state state{};
    std::optional<std::string> stage{};
    std::optional<runtime_end_reason> end_reason{};
    std::optional<std::string> error{};
};

using runtime_event_handler = std::function<void(runtime_event)>;

class runtime_event_emitter final
{
   public:
    runtime_event_emitter(std::string server_id, std::string instance_id, runtime_event_handler handler)
        : server_id_(std::move(server_id)), instance_id_(std::move(instance_id)), handler_(std::move(handler))
    {
    }

    void emit(runtime_event event) const
    {
        if (!handler_)
        {
            return;
        }
        event.server_id = server_id_;
        event.instance_id = instance_id_;
        handler_(std::move(event));
    }

   private:
    std::string server_id_;
    std::string instance_id_;
    runtime_event_handler handler_;
};

using runtime_event_emitter_ptr = std::shared_ptr<const runtime_event_emitter>;

[[nodiscard]] constexpr std::string_view to_string(runtime_kind value) noexcept
{
    switch (value)
    {
        case runtime_kind::source:
            return "source";
        case runtime_kind::publisher:
            return "publisher";
        case runtime_kind::output:
            return "output";
    }
    return {};
}

[[nodiscard]] constexpr std::string_view to_string(runtime_protocol value) noexcept
{
    switch (value)
    {
        case runtime_protocol::rtmp:
            return "rtmp";
        case runtime_protocol::rtsp:
            return "rtsp";
        case runtime_protocol::gb28181:
            return "gb28181";
        case runtime_protocol::whep:
            return "whep";
    }
    return {};
}

[[nodiscard]] constexpr std::string_view to_string(runtime_state value) noexcept
{
    switch (value)
    {
        case runtime_state::starting:
            return "starting";
        case runtime_state::streaming:
            return "streaming";
        case runtime_state::stopped:
            return "stopped";
    }
    return {};
}

[[nodiscard]] constexpr std::string_view to_string(runtime_end_reason value) noexcept
{
    switch (value)
    {
        case runtime_end_reason::requested:
            return "requested";
        case runtime_end_reason::remote:
            return "remote";
        case runtime_end_reason::timeout:
            return "timeout";
        case runtime_end_reason::protocol_error:
            return "protocol_error";
        case runtime_end_reason::runtime_error:
            return "runtime_error";
        case runtime_end_reason::server_shutdown:
            return "server_shutdown";
    }
    return {};
}

}    // namespace media_server

#endif
