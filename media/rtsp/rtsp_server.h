#ifndef MEDIA_RTSP_RTSP_SERVER_H
#define MEDIA_RTSP_RTSP_SERVER_H

#include <memory>
#include <cstdint>

#include <boost/asio/io_context.hpp>
#include <boost/system/error_code.hpp>

#include "config.h"
#include "media/net/tcp_listener.h"

namespace media_server
{
class rtsp_server_connection;

class rtsp_server final : public std::enable_shared_from_this<rtsp_server>
{
   public:
    rtsp_server(io_context_pool& workers, const config& config);

    [[nodiscard]] boost::system::error_code startup();

   private:
    void on_accept(boost::asio::ip::tcp::socket socket);

    const config& config_;
    std::shared_ptr<tcp_listener> listener_;
};

}    // namespace media_server

#endif
