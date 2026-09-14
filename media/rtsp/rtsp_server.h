#ifndef MEDIA_RTSP_RTSP_SERVER_H
#define MEDIA_RTSP_RTSP_SERVER_H

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
class rtsp_server_connection;
class signaling_client;

class rtsp_server final : public std::enable_shared_from_this<rtsp_server>
{
   public:
    rtsp_server(io_context_pool& workers, const config& config, std::shared_ptr<signaling_client> signaling = {});

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
    std::vector<std::weak_ptr<rtsp_server_connection>> sessions_;
    bool closed_{};
};

}    // namespace media_server

#endif
