#ifndef MEDIA_RTSP_RTSP_SERVER_H
#define MEDIA_RTSP_RTSP_SERVER_H

#include <mutex>
#include <memory>
#include <vector>
#include <cstdint>

#include <boost/asio/io_context.hpp>
#include <boost/system/error_code.hpp>

#include "media/net/tcp_listener.h"
#include "media/codec/output_video_config.h"

namespace media_server
{
class rtsp_server_connection;

class rtsp_server final : public std::enable_shared_from_this<rtsp_server>
{
   public:
    rtsp_server(io_context_pool& workers, std::uint16_t port, output_video_config video = {});

    [[nodiscard]] boost::system::error_code startup();
    void shutdown();

   private:
    void on_accept(boost::asio::ip::tcp::socket socket);

    output_video_config video_config_;
    std::shared_ptr<tcp_listener> listener_;
    std::mutex sessions_mutex_;
    std::vector<std::weak_ptr<rtsp_server_connection>> connections_;
    bool closed_{};
};

}    // namespace media_server

#endif
