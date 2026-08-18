#ifndef MEDIA_RTSP_RTSP_SERVER_H
#define MEDIA_RTSP_RTSP_SERVER_H

#include "media/codec/output_video_config.h"
#include "media/core/stream_registry.h"
#include "media/net/tcp_listener.h"

#include <boost/asio/io_context.hpp>
#include <boost/system/error_code.hpp>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace media_server
{
class rtsp_server_connection;
class rtsp_connection_router;

class rtsp_server final : public std::enable_shared_from_this<rtsp_server>
{
   public:
    rtsp_server(io_context_pool& workers, stream_registry& registry, std::uint16_t port, output_video_config video = {});

    [[nodiscard]] boost::system::error_code startup();
    void shutdown();

   private:
    friend class rtsp_connection_router;
    void on_accept(boost::asio::ip::tcp::socket socket);
    void on_connection(boost::asio::ip::tcp::socket socket, std::vector<std::uint8_t> initial_data, bool publish);

    stream_registry& registry_;
    output_video_config video_config_;
    std::shared_ptr<tcp_listener> listener_;
    std::mutex sessions_mutex_;
    std::vector<std::weak_ptr<rtsp_connection_router>> routers_;
    std::vector<std::weak_ptr<rtsp_server_connection>> connections_;
    bool closed_{};
};

}    // namespace media_server

#endif
