#ifndef MEDIA_SERVER_SERVICE_H
#define MEDIA_SERVER_SERVICE_H

#include <memory>

#include <boost/asio/steady_timer.hpp>

#include "config.h"

namespace media_server
{

class http_server;
class io_context_pool;
class signaling_client;
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
    void schedule_signaling_abort();

    config config_;
    std::unique_ptr<io_context_pool> workers_;
    std::shared_ptr<rtmp_server> rtmp_;
    std::shared_ptr<rtsp_server> rtsp_;
    std::shared_ptr<http_server> http_;
    std::shared_ptr<signaling_client> signaling_;
    std::unique_ptr<boost::asio::steady_timer> signaling_abort_timer_;
};

}    // namespace media_server

#endif
