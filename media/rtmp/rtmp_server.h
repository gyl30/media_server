#ifndef MEDIA_RTMP_RTMP_SERVER_H
#define MEDIA_RTMP_RTMP_SERVER_H

#include "media/core/stream_registry.h"
#include "media/net/tcp_listener.h"

#include <boost/asio/io_context.hpp>
#include <boost/system/error_code.hpp>
#include <cstdint>
#include <memory>
#include <vector>

namespace media_server
{
class rtmp_session;

class rtmp_server final
{
   public:
    rtmp_server(boost::asio::io_context& io, stream_registry& registry, std::uint16_t port);

    [[nodiscard]] boost::system::error_code start();
    void close();

   private:
    stream_registry& registry_;
    tcp_listener listener_;
    std::vector<std::weak_ptr<rtmp_session>> sessions_;
};

}    // namespace media_server

#endif
