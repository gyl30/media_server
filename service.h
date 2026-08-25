#ifndef MEDIA_SERVER_SERVICE_H
#define MEDIA_SERVER_SERVICE_H

#include <memory>
#include <vector>

#include "config.h"

namespace media_server
{

class gb28181_service;
class http_server;
class io_context_pool;
class rtmp_server;
class rtsp_pull_session;
class rtsp_server;
class whep_service;

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
    std::unique_ptr<whep_service> whep_;
    std::unique_ptr<gb28181_service> gb28181_;
    std::shared_ptr<rtmp_server> rtmp_;
    std::shared_ptr<rtsp_server> rtsp_;
    std::shared_ptr<http_server> http_;
    std::vector<std::weak_ptr<rtsp_pull_session>> pulls_;
};

}    // namespace media_server

#endif
