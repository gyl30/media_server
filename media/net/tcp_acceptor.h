#ifndef MEDIA_NET_TCP_ACCEPTOR_H
#define MEDIA_NET_TCP_ACCEPTOR_H

#include <chrono>
#include <memory>
#include <cstdint>

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/system/error_code.hpp>
#include <boost/asio/any_io_executor.hpp>

#include "media/net/tcp_socket_source.h"

namespace media_server
{

class tcp_acceptor final : public tcp_socket_source, public std::enable_shared_from_this<tcp_acceptor>
{
   public:
    tcp_acceptor(boost::asio::any_io_executor executor, std::uint16_t port, boost::asio::ip::address bind_address, std::chrono::milliseconds timeout);

    [[nodiscard]] boost::system::error_code startup(socket_handler handler) override;
    void shutdown() override;

   private:
    void accept_next();
    void complete(boost::system::error_code error);
    void complete(boost::system::error_code error, boost::asio::ip::tcp::socket socket);
    void safe_shutdown();

    boost::asio::ip::tcp::acceptor acceptor_;
    boost::asio::steady_timer timer_;
    boost::asio::ip::address bind_address_;
    std::uint16_t port_{};
    std::chrono::milliseconds timeout_{};
    socket_handler socket_handler_;
    bool started_{};
    bool completed_{};
};

}    // namespace media_server

#endif
