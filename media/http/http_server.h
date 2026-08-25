#ifndef MEDIA_HTTP_HTTP_SERVER_H
#define MEDIA_HTTP_HTTP_SERVER_H

#include <mutex>
#include <memory>
#include <vector>
#include <cstdint>

#include <boost/asio/io_context.hpp>
#include <boost/system/error_code.hpp>

#include "media/hls/hls_service.h"
#include "media/net/tcp_listener.h"
#include "media/webrtc/whep_service.h"
#include "media/gb28181/gb28181_service.h"
#include "media/codec/output_video_config.h"

namespace media_server
{
class http_session;

class http_server final : public std::enable_shared_from_this<http_server>
{
   public:
    http_server(io_context_pool& workers,
                hls_service& hls,
                whep_service& whep,
                gb28181_service& gb28181,
                std::uint16_t port,
                output_video_config video = {});

    [[nodiscard]] boost::system::error_code startup();
    void shutdown();

   private:
    void on_accept(boost::asio::ip::tcp::socket socket);

    hls_service& hls_;
    whep_service& whep_;
    gb28181_service& gb28181_;
    output_video_config video_config_;
    std::shared_ptr<tcp_listener> listener_;
    std::mutex sessions_mutex_;
    std::vector<std::weak_ptr<http_session>> sessions_;
    bool closed_{};
};
}    // namespace media_server

#endif
