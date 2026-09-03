#ifndef MEDIA_NET_TCP_LISTENER_H
#define MEDIA_NET_TCP_LISTENER_H

#include <chrono>
#include <memory>
#include <cstddef>
#include <cstdint>
#include <functional>

#include <boost/asio.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/system/error_code.hpp>

#include "media/net/io_context_pool.h"

namespace media_server
{

class tcp_listener final : public std::enable_shared_from_this<tcp_listener>
{
   public:
    using accept_handler = std::function<void(boost::system::error_code, worker_context&, boost::asio::ip::tcp::socket)>;

    tcp_listener(io_context_pool& workers, std::uint16_t port, boost::asio::ip::address bind_address);

    void startup(accept_handler handler, std::chrono::milliseconds timeout, boost::system::error_code& ec);
    void shutdown();

   private:
    void accept_loop(std::chrono::milliseconds timeout, boost::asio::yield_context& yield);
    void on_timeout(const boost::system::error_code& ec);
    void safe_shutdown();

   private:
    worker_context& worker_;
    boost::asio::steady_timer timer_;
    boost::asio::ip::tcp::acceptor acceptor_;
    std::uint16_t port_{};
    boost::asio::ip::address bind_address_;
    io_context_pool& workers_;
    accept_handler accept_handler_;
};

}    // namespace media_server

#endif
