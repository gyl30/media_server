#ifndef MEDIA_NET_TCP_LISTENER_H
#define MEDIA_NET_TCP_LISTENER_H

#include <chrono>
#include <cstdint>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/system/error_code.hpp>

namespace media_server
{

class tcp_listener final
{
   public:
    tcp_listener(boost::asio::io_context& io, std::uint16_t port, boost::asio::ip::address bind_address);

    void startup(boost::system::error_code& error);
    void accept(boost::asio::ip::tcp::socket& socket,
                std::chrono::milliseconds timeout,
                boost::asio::yield_context& yield,
                boost::system::error_code& error);
    void shutdown();

   private:
    boost::asio::steady_timer timer_;
    boost::asio::ip::tcp::acceptor acceptor_;
    std::uint16_t port_{};
    boost::asio::ip::address bind_address_;
    bool closed_{};
};

}    // namespace media_server

#endif
