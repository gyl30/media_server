#ifndef MEDIA_RTMP_RTMP_SERVER_H
#define MEDIA_RTMP_RTMP_SERVER_H

#include <memory>
#include <vector>
#include <mutex>
#include <cstdint>

#include <boost/asio/spawn.hpp>
#include <boost/system/error_code.hpp>

#include "config.h"
#include "media/net/tcp_listener.h"
#include "media/net/io_context_pool.h"

namespace media_server
{
class rtmp_session;
class signaling_client;

class rtmp_server final : public std::enable_shared_from_this<rtmp_server>
{
   public:
    rtmp_server(io_context_pool& workers, const config& config, std::shared_ptr<signaling_client> signaling = {});

    void startup(boost::system::error_code& error);
    void shutdown();

   private:
    void run(boost::asio::yield_context yield);
    void safe_shutdown();

    io_context_pool& workers_;
    worker_context& worker_;
    const config& config_;
    std::shared_ptr<signaling_client> signaling_;
    tcp_listener listener_;
    std::mutex mutex_;
    std::vector<std::weak_ptr<rtmp_session>> sessions_;
    bool closed_{};
};

}    // namespace media_server

#endif
