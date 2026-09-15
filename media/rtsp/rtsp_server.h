#ifndef MEDIA_RTSP_RTSP_SERVER_H
#define MEDIA_RTSP_RTSP_SERVER_H

#include <memory>

#include <boost/asio/spawn.hpp>
#include <boost/system/error_code.hpp>

#include "config.h"
#include "media/core/runtime_event.h"
#include "media/net/tcp_listener.h"
#include "media/net/io_context_pool.h"

namespace media_server
{
class signaling_client;

class rtsp_server final : public std::enable_shared_from_this<rtsp_server>
{
   public:
    rtsp_server(io_context_pool& workers,
                const config& config,
                std::shared_ptr<signaling_client> signaling = {},
                runtime_event_emitter_ptr runtime_events = {});

    void startup(boost::system::error_code& error);

   private:
    void run(boost::asio::yield_context yield);

    io_context_pool& workers_;
    worker_context& worker_;
    const config& config_;
    std::shared_ptr<signaling_client> signaling_;
    runtime_event_emitter_ptr runtime_events_;
    tcp_listener listener_;
};

}    // namespace media_server

#endif
