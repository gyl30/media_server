#ifndef MEDIA_HTTP_HTTP_SERVER_H
#define MEDIA_HTTP_HTTP_SERVER_H

#include <memory>
#include <cstdint>

#include <boost/asio/io_context.hpp>
#include <boost/system/error_code.hpp>

#include "config.h"
#include "media/net/tcp_listener.h"

namespace media_server
{
class http_session;

class http_server final : public std::enable_shared_from_this<http_server>
{
   public:
    http_server(io_context_pool& workers, const config& config);

    [[nodiscard]] boost::system::error_code startup();

   private:
    void on_accept(boost::asio::ip::tcp::socket socket);

    io_context_pool& workers_;
    const config& config_;
    std::shared_ptr<tcp_listener> listener_;
};
}    // namespace media_server

#endif
