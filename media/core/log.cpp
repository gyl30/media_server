#include "media/core/log.h"

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <string_view>

namespace media_server
{

void configure_log_level()
{
    const auto* value = std::getenv("MEDIA_SERVER_LOG_LEVEL");
    if (value == nullptr || *value == '\0')
    {
        spdlog::set_level(spdlog::level::info);
        return;
    }

    const std::string_view level(value);
    if (level == "trace")
    {
        spdlog::set_level(spdlog::level::trace);
    }
    else if (level == "debug")
    {
        spdlog::set_level(spdlog::level::debug);
    }
    else if (level == "info")
    {
        spdlog::set_level(spdlog::level::info);
    }
    else if (level == "warn")
    {
        spdlog::set_level(spdlog::level::warn);
    }
    else if (level == "error")
    {
        spdlog::set_level(spdlog::level::err);
    }
    else if (level == "critical")
    {
        spdlog::set_level(spdlog::level::critical);
    }
    else if (level == "off")
    {
        spdlog::set_level(spdlog::level::off);
    }
    else
    {
        spdlog::set_level(spdlog::level::info);
        spdlog::warn("unknown log level {} use info", level);
    }
}

}    // namespace media_server
