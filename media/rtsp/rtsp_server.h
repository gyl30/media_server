#ifndef MEDIA_RTSP_RTSP_SERVER_H
#define MEDIA_RTSP_RTSP_SERVER_H

#include <memory>

#include <boost/asio/spawn.hpp>
#include <boost/system/error_code.hpp>

#include "config.h"
#include "media/net/tcp_listener.h"
#include "media/net/io_context_pool.h"

namespace media_server
{
class rtsp_server final : public std::enable_shared_from_this<rtsp_server>
{
   public:
    rtsp_server(io_context_pool& workers, const config& config);

   public:
    void startup(boost::system::error_code& error);

   private:
    void run(boost::asio::yield_context yield);

   private:
    io_context_pool& workers_;
    worker_context& worker_;
    const config& config_;
    tcp_listener listener_;
};

}    // namespace media_server

#endif
