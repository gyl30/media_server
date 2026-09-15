#ifndef MEDIA_HTTP_HTTP_SERVER_H
#define MEDIA_HTTP_HTTP_SERVER_H

#include <memory>

#include <boost/asio/spawn.hpp>
#include <boost/system/error_code.hpp>

#include "config.h"
#include "media/core/runtime_event.h"
#include "media/net/tcp_listener.h"
#include "media/net/io_context_pool.h"

namespace media_server
{
class http_server final : public std::enable_shared_from_this<http_server>
{
   public:
    http_server(io_context_pool& workers, const config& config, runtime_event_emitter_ptr runtime_events = {});

    void startup(boost::system::error_code& error);

   private:
    void run(boost::asio::yield_context yield);

    io_context_pool& workers_;
    worker_context& worker_;
    const config& config_;
    runtime_event_emitter_ptr runtime_events_;
    tcp_listener listener_;
};
}    // namespace media_server

#endif
