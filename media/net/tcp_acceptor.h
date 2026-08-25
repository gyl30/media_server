#ifndef MEDIA_NET_TCP_ACCEPTOR_H
#define MEDIA_NET_TCP_ACCEPTOR_H

#include <chrono>
#include <functional>
#include <memory>
#include <cstdint>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/system/error_code.hpp>

namespace media_server
{

class tcp_acceptor final : public std::enable_shared_from_this<tcp_acceptor>
{
   public:
    using accept_handler = std::function<void(boost::asio::ip::tcp::socket)>;

    tcp_acceptor(boost::asio::any_io_executor executor, std::uint16_t port, boost::asio::ip::address bind_address);

    [[nodiscard]] boost::system::error_code startup(accept_handler handler, std::chrono::milliseconds timeout = {});
    void shutdown();

   private:
    void accept_next();
    void safe_shutdown();

    boost::asio::ip::tcp::acceptor acceptor_;
    boost::asio::steady_timer timer_;
    boost::asio::ip::address bind_address_;
    std::uint16_t port_{};
    accept_handler handler_;
    bool started_{};
};

}    // namespace media_server

#endif
