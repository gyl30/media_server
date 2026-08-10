#ifndef MEDIA_HTTP_HTTP_SERVER_H
#define MEDIA_HTTP_HTTP_SERVER_H

#include "media/core/stream_registry.h"
#include "media/hls/hls_service.h"
#include "media/net/tcp_listener.h"
#include "media/webrtc/whep_service.h"

#include <boost/asio/io_context.hpp>
#include <boost/system/error_code.hpp>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace media_server
{
class http_session;

class http_server final
{
   public:
    http_server(io_context_pool& workers, stream_registry& registry, hls_service& hls, whep_service& whep, std::uint16_t port);

    [[nodiscard]] boost::system::error_code start();
    void close();

   private:
    stream_registry& registry_;
    hls_service& hls_;
    whep_service& whep_;
    tcp_listener listener_;
    std::mutex sessions_mutex_;
    std::vector<std::weak_ptr<http_session>> sessions_;
    bool closed_{};
};
}    // namespace media_server

#endif
