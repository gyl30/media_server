#ifndef MEDIA_SERVER_SERVICE_H
#define MEDIA_SERVER_SERVICE_H

#include <memory>

#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/signal_set.hpp>

#include <boost/asio/spawn.hpp>
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
    void run_control(boost::asio::yield_context yield);
    void stop();
    void schedule_signaling_abort();

    config config_;
    std::unique_ptr<io_context_pool> workers_;
    std::shared_ptr<rtmp_server> rtmp_;
    std::shared_ptr<rtsp_server> rtsp_;
    std::shared_ptr<http_server> http_;
    std::shared_ptr<signaling_client> signaling_;
    std::unique_ptr<boost::asio::steady_timer> signaling_abort_timer_;
    std::unique_ptr<boost::asio::signal_set> signals_;
    boost::asio::cancellation_signal control_cancellation_;
    std::size_t pending_shutdown_workers_{};
    bool stopping_{};
    int exit_code_{};
};

}    // namespace media_server

#endif
