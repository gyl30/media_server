#ifndef MEDIA_NET_TCP_LISTENER_H
#define MEDIA_NET_TCP_LISTENER_H

#include "media/net/io_context_pool.h"

#include <boost/asio.hpp>
#include <boost/system/error_code.hpp>

#include <cstdint>
#include <functional>

namespace media_server
{

class tcp_listener final
{
   public:
    using accept_handler = std::function<void(boost::asio::ip::tcp::socket)>;

    tcp_listener(io_context_pool& workers, std::uint16_t port, accept_handler handler);

    [[nodiscard]] boost::system::error_code start();
    void close();

   private:
    void accept_next();

    boost::asio::ip::tcp::acceptor acceptor_;
    io_context_pool& workers_;
    std::uint16_t port_{};
    accept_handler handler_;
    bool started_{};
};

}    // namespace media_server

#endif
