#include <cstdlib>
#include <utility>

#include "config.h"
#include "service.h"
#include "media/net/port_manager.h"

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
    media_server::service service(std::move(cfg));
    const int service_result = service.run();
    if (service.stopped_by_signal())
    {
        // Hard stop leaves deferred stackful coroutine handlers in their io_contexts.
        std::_Exit(service_result);
    }
    return service_result;
}
