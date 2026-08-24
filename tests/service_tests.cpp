#include <iostream>
#include <stdexcept>
#include <utility>

#include "config.h"
#include "service.h"

namespace
{

void require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

}    // namespace

int main()
{
    media_server::config cfg;
    cfg.webrtc_address = "invalid-address";

    media_server::service service(std::move(cfg));
    require(service.run() == 1, "service rejects invalid webrtc address");

    std::cout << "[pass] service tests\n";
    return 0;
}
