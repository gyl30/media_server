#ifndef MEDIA_SERVER_SERVICE_H
#define MEDIA_SERVER_SERVICE_H

#include <memory>

#include <boost/asio/spawn.hpp>
#include <boost/asio/signal_set.hpp>

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
    bool register_signaling(boost::asio::yield_context& yield);
    void run_control(boost::asio::yield_context yield);
    void stop();

   private:
    config config_;
    std::unique_ptr<io_context_pool> workers_;
    std::shared_ptr<rtmp_server> rtmp_;
    std::shared_ptr<rtsp_server> rtsp_;
    std::shared_ptr<http_server> http_;
    std::unique_ptr<boost::asio::signal_set> signals_;
    int exit_code_{};
};

}    // namespace media_server

#endif
