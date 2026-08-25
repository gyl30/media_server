#ifndef MEDIA_SERVER_SERVICE_H
#define MEDIA_SERVER_SERVICE_H

#include <memory>

#include "config.h"

namespace media_server
{

class http_server;
class io_context_pool;
class rtmp_server;
class rtsp_server;

class service
{
public:
    explicit service(config cfg);
    ~service();

    int run();

private:
    void stop();

    config config_;
    std::unique_ptr<io_context_pool> workers_;
    std::shared_ptr<rtmp_server> rtmp_;
    std::shared_ptr<rtsp_server> rtsp_;
    std::shared_ptr<http_server> http_;
};

}    // namespace media_server

#endif
