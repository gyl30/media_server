#include <utility>

#include "config.h"
#include "media/core/stream_registry.h"
#include "media/net/port_manager.h"
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

    media_server::port_manager::init(media_server::default_media_port_start, media_server::default_media_port_end);
    media_server::registry::init();
    int service_result = 0;
    {
        media_server::service service(std::move(cfg));
        service_result = service.run();
    }
    media_server::registry::destroy();
    media_server::port_manager::destroy();
    return service_result;
}
