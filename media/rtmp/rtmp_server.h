#ifndef MEDIA_RTMP_RTMP_SERVER_H
#define MEDIA_RTMP_RTMP_SERVER_H

#include <memory>
#include <mutex>
#include <cstdint>

#include <boost/asio/io_context.hpp>
#include <boost/system/error_code.hpp>

#include "config.h"
#include "media/net/tcp_listener.h"

namespace media_server
{
class rtmp_session;

class rtmp_server final : public std::enable_shared_from_this<rtmp_server>
{
   public:
    rtmp_server(io_context_pool& workers, const config& config);

    void startup(boost::system::error_code& error);
    void shutdown();

   private:
    void on_accept(boost::asio::ip::tcp::socket socket);

    const config& config_;
    std::shared_ptr<tcp_listener> listener_;
    std::mutex mutex_;
    bool closed_{};
};

}    // namespace media_server

#endif
