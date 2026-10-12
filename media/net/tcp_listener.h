#ifndef MEDIA_NET_TCP_LISTENER_H
#define MEDIA_NET_TCP_LISTENER_H

#include <cstdint>

#include <boost/asio/spawn.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/system/error_code.hpp>

namespace media_server
{

class tcp_listener final
{
   public:
    explicit tcp_listener(boost::asio::io_context& io);

   public:
    void startup(const boost::asio::ip::address& bind_address, std::uint16_t port, boost::system::error_code& error);
    void accept(boost::asio::ip::tcp::socket& socket, boost::asio::yield_context& yield, boost::system::error_code& error);
    void shutdown();

   private:
    boost::asio::ip::tcp::acceptor acceptor_;
};

}    // namespace media_server

#endif
