#include <utility>

#include "config.h"
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

    media_server::service service(std::move(cfg));
    return service.run();
}
