#ifndef MEDIA_NET_TCP_CONNECTOR_H
#define MEDIA_NET_TCP_CONNECTOR_H

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/system/error_code.hpp>

#include <chrono>
#include <functional>
#include <memory>

namespace media_server
{

class tcp_connector final : public std::enable_shared_from_this<tcp_connector>
{
   public:
    using connect_handler = std::function<void(boost::system::error_code, boost::asio::ip::tcp::socket)>;

    explicit tcp_connector(boost::asio::any_io_executor executor);

    void startup(boost::asio::ip::tcp::endpoint endpoint, std::chrono::milliseconds timeout, connect_handler handler);
    void shutdown();

   private:
    void complete(boost::system::error_code error);
    void safe_shutdown();

    boost::asio::ip::tcp::socket socket_;
    boost::asio::steady_timer timer_;
    connect_handler handler_;
    bool completed_{};
};

}    // namespace media_server

#endif
