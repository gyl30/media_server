#ifndef MEDIA_SERVER_SERVICE_H
#define MEDIA_SERVER_SERVICE_H

#include "config.h"

namespace media_server
{

class service
{
public:
    explicit service(config cfg);

    int run();

private:
    config config_;
};

}    // namespace media_server

#endif
