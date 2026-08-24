#ifndef MEDIA_RTMP_RTMP_SERVER_H
#define MEDIA_RTMP_RTMP_SERVER_H

#include <mutex>
#include <memory>
#include <vector>
#include <cstdint>

#include <boost/asio/io_context.hpp>
#include <boost/system/error_code.hpp>

#include "media/net/tcp_listener.h"
#include "media/core/stream_registry.h"
#include "media/codec/output_video_config.h"

namespace media_server
{
class rtmp_session;

class rtmp_server final : public std::enable_shared_from_this<rtmp_server>
{
   public:
    rtmp_server(io_context_pool& workers, stream_registry& registry, std::uint16_t port, output_video_config video = {});

    [[nodiscard]] boost::system::error_code startup();
    void shutdown();

   private:
    void on_accept(boost::asio::ip::tcp::socket socket);

    stream_registry& registry_;
    output_video_config video_config_;
    std::shared_ptr<tcp_listener> listener_;
    std::mutex sessions_mutex_;
    std::vector<std::weak_ptr<rtmp_session>> sessions_;
    bool closed_{};
};

}    // namespace media_server

#endif
