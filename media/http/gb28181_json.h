#ifndef MEDIA_HTTP_GB28181_JSON_H
#define MEDIA_HTTP_GB28181_JSON_H

#include <string>
#include <utility>
#include <optional>
#include <string_view>

#include "media/gb28181/gb28181_types.h"

namespace media_server
{

struct gb28181_receiver_config
{
    std::string stream_id;
    std::string stream_name;
    gb28181_transport_config transport;
};

struct gb28181_sender_config
{
    std::string stream_id;
    std::string stream_name;
    std::string sender_id;
    gb28181_transport_config transport;
    bool rtcp_enabled{};
};

struct gb28181_receiver_identity
{
    std::string stream_id;
    std::string stream_name;
};

struct gb28181_sender_identity
{
    std::string stream_id;
    std::string stream_name;
    std::string sender_id;
};

[[nodiscard]] std::optional<gb28181_receiver_config> parse_gb28181_receiver_config(std::string_view body);
[[nodiscard]] std::optional<gb28181_sender_config> parse_gb28181_sender_config(std::string_view body);
[[nodiscard]] std::optional<gb28181_receiver_identity> parse_gb28181_receiver_delete(std::string_view body);
[[nodiscard]] std::optional<gb28181_sender_identity> parse_gb28181_sender_delete(std::string_view body);

}    // namespace media_server

#endif
