#ifndef MEDIA_HTTP_HTTP_SERVER_H
#define MEDIA_HTTP_HTTP_SERVER_H

#include <memory>
#include <vector>
#include <mutex>

#include <boost/asio/spawn.hpp>
#include <boost/system/error_code.hpp>

#include "config.h"
#include "media/core/runtime_event.h"
#include "media/net/tcp_listener.h"
#include "media/net/io_context_pool.h"

namespace media_server
{
class http_session;

class http_server final : public std::enable_shared_from_this<http_server>
{
   public:
    http_server(io_context_pool& workers, const config& config, runtime_event_emitter_ptr runtime_events = {});

    void startup(boost::system::error_code& error);
    void shutdown();

   private:
    void run(boost::asio::yield_context yield);
    void safe_shutdown();

    io_context_pool& workers_;
    worker_context& worker_;
    const config& config_;
    runtime_event_emitter_ptr runtime_events_;
    tcp_listener listener_;
    std::mutex mutex_;
    std::vector<std::weak_ptr<http_session>> sessions_;
    bool closed_{};
};
}    // namespace media_server

#endif
