#ifndef MEDIA_HTTP_GB28181_JSON_H
#define MEDIA_HTTP_GB28181_JSON_H

#include <string>
#include <utility>
#include <optional>
#include <string_view>

#include "media/gb28181/gb28181_types.h"

namespace media_server
{

[[nodiscard]] std::optional<gb28181_receiver_config> parse_gb28181_receiver_config(std::string_view body);
[[nodiscard]] std::optional<gb28181_sender_config> parse_gb28181_sender_config(std::string_view body);
[[nodiscard]] std::optional<gb28181_receiver_identity> parse_gb28181_receiver_delete(std::string_view body);
[[nodiscard]] std::optional<gb28181_sender_identity> parse_gb28181_sender_delete(std::string_view body);

}    // namespace media_server

#endif
