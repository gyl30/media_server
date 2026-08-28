#ifndef MEDIA_NET_TCP_CONNECTOR_H
#define MEDIA_NET_TCP_CONNECTOR_H

#include <chrono>
#include <memory>

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/system/error_code.hpp>
#include <boost/asio/any_io_executor.hpp>

#include "media/net/tcp_socket_source.h"

namespace media_server
{

class tcp_connector final : public tcp_socket_source, public std::enable_shared_from_this<tcp_connector>
{
   public:
    tcp_connector(boost::asio::any_io_executor executor, boost::asio::ip::tcp::endpoint endpoint, std::chrono::milliseconds timeout);

    void startup(socket_handler handler, boost::system::error_code& error) override;
    void shutdown() override;

   private:
    void complete(boost::system::error_code error);
    void safe_shutdown();

    boost::asio::ip::tcp::socket socket_;
    boost::asio::steady_timer timer_;
    boost::asio::ip::tcp::endpoint endpoint_;
    std::chrono::milliseconds timeout_{};
    socket_handler socket_handler_;
    bool started_{};
    bool completed_{};
};

}    // namespace media_server

#endif
