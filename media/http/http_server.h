#ifndef MEDIA_HTTP_HTTP_SERVER_H
#define MEDIA_HTTP_HTTP_SERVER_H

#include <mutex>
#include <memory>
#include <vector>
#include <cstdint>

#include <boost/asio/io_context.hpp>
#include <boost/system/error_code.hpp>

#include "config.h"
#include "media/net/tcp_listener.h"
#include "media/gb28181/gb28181_service.h"

namespace media_server
{
class http_session;

class http_server final : public std::enable_shared_from_this<http_server>
{
   public:
    http_server(io_context_pool& workers,
                const config& config,
                gb28181_service& gb28181);

    [[nodiscard]] boost::system::error_code startup();
    void shutdown();

   private:
    void on_accept(boost::asio::ip::tcp::socket socket);

    const config& config_;
    gb28181_service& gb28181_;
    std::shared_ptr<tcp_listener> listener_;
    std::mutex sessions_mutex_;
    std::vector<std::weak_ptr<http_session>> sessions_;
    bool closed_{};
};
}    // namespace media_server

#endif
