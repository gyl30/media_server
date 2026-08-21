#ifndef MEDIA_NET_TCP_LISTENER_H
#define MEDIA_NET_TCP_LISTENER_H

#include "media/net/io_context_pool.h"

#include <boost/asio.hpp>
#include <boost/system/error_code.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace media_server
{

class tcp_listener final : public std::enable_shared_from_this<tcp_listener>
{
   public:
    using accept_handler = std::function<void(boost::asio::ip::tcp::socket)>;

    tcp_listener(io_context_pool& workers, std::uint16_t port);

    [[nodiscard]] boost::system::error_code startup(accept_handler handler, std::size_t accept_limit = 0);
    void shutdown();

   private:
    void safe_shutdown();
    void accept_next();

    boost::asio::ip::tcp::acceptor acceptor_;
    io_context_pool& workers_;
    std::uint16_t port_{};
    accept_handler handler_;
    std::size_t accept_limit_{};
    std::size_t accepted_count_{};
    bool started_{};
};

}    // namespace media_server

#endif
