#include <utility>

#include "config.h"
#include "service.h"
#include "media/net/media_port_pool.h"

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

    media_server::media_port_pool::init(media_server::default_media_port_start, media_server::default_media_port_end);
    media_server::service service(std::move(cfg));
    return service.run();
}
