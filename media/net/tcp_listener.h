#ifndef MEDIA_NET_TCP_LISTENER_H
#define MEDIA_NET_TCP_LISTENER_H

#include <chrono>
#include <memory>
#include <cstddef>
#include <cstdint>
#include <functional>

#include <boost/asio.hpp>
#include <boost/system/error_code.hpp>

#include "media/net/io_context_pool.h"

namespace media_server
{

class tcp_listener final : public std::enable_shared_from_this<tcp_listener>
{
   public:
    using accept_handler = std::function<void(boost::system::error_code, boost::asio::ip::tcp::socket)>;

    tcp_listener(io_context_pool& workers, std::uint16_t port, boost::asio::ip::address bind_address);

    void startup(accept_handler handler, std::size_t accept_limit, std::chrono::milliseconds timeout, boost::system::error_code& error);
    void shutdown();

   private:
    void schedule_timeout();
    void safe_shutdown();
    void accept_next();

    boost::asio::ip::tcp::acceptor acceptor_;
    boost::asio::steady_timer timer_;
    io_context_pool& workers_;
    boost::asio::ip::address bind_address_;
    std::uint16_t port_{};
    accept_handler accept_handler_;
    std::chrono::milliseconds timeout_{};
    std::size_t accept_limit_{};
    std::size_t accepted_count_{};
    bool started_{};
    bool accepting_{};
};

}    // namespace media_server

#endif
