#include <utility>

#include "config.h"
#include "media/core/stream_registry.h"
#include "service.h"

int main(int argc, char** argv)
{
    media_server::config cfg;
    const int result = media_server::parse_config(argc, argv, &cfg);
    if (result != 0)
    {
        return result;
    }
    if (cfg.help)
    {
        return 0;
    }

    media_server::registry::init();
    media_server::service service(std::move(cfg));
    const int service_result = service.run();
    media_server::registry::destroy();
    return service_result;
}
