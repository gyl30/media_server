#ifndef MEDIA_HTTP_GB28181_JSON_H
#define MEDIA_HTTP_GB28181_JSON_H

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "media/gb28181/gb28181_types.h"

namespace media_server
{

[[nodiscard]] std::optional<gb28181_input_config> parse_gb28181_input_config(std::string_view body);
[[nodiscard]] std::optional<gb28181_output_config> parse_gb28181_output_config(std::string_view body);
[[nodiscard]] std::optional<std::string> parse_gb28181_input_delete(std::string_view body);
[[nodiscard]] std::optional<std::pair<std::string, std::string>> parse_gb28181_output_delete(std::string_view body);

}    // namespace media_server

#endif
