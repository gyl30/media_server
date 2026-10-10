#ifndef MEDIA_SERVER_HTTP_TARGET_H
#define MEDIA_SERVER_HTTP_TARGET_H

#include <optional>
#include <string_view>

#include <ada.h>

namespace media_server
{

[[nodiscard]] std::optional<ada::url_aggregator> parse_http_target(std::string_view target);

}    // namespace media_server

#endif
