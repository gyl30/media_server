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

enum class accept_error_action
{
    retry_now,
    retry_later,
    fatal,
};

[[nodiscard]] accept_error_action classify_accept_error(const boost::system::error_code& error) noexcept;

class tcp_listener final
{
   public:
    tcp_listener(boost::asio::io_context& io, std::uint16_t port, boost::asio::ip::address bind_address);

   public:
    void startup(boost::system::error_code& error);
    void accept(boost::asio::ip::tcp::socket& socket, boost::asio::yield_context& yield, boost::system::error_code& error);
    void shutdown();

   private:
    boost::asio::ip::tcp::acceptor acceptor_;
    std::uint16_t port_{};
    boost::asio::ip::address bind_address_;
};

}    // namespace media_server

#endif
