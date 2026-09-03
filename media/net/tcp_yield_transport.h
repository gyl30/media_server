#ifndef MEDIA_NET_TCP_YIELD_TRANSPORT_H
#define MEDIA_NET_TCP_YIELD_TRANSPORT_H

#include <span>
#include <cstddef>
#include <cstdint>

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/system/error_code.hpp>

namespace media_server
{

class tcp_yield_transport final
{
   public:
    explicit tcp_yield_transport(boost::asio::ip::tcp::socket socket);

    std::size_t read(std::span<std::uint8_t> buffer, boost::asio::yield_context& yield, boost::system::error_code& error);
    std::size_t write(std::span<const std::uint8_t> data, boost::asio::yield_context& yield, boost::system::error_code& error);

    [[nodiscard]] boost::asio::ip::tcp::endpoint local_endpoint(boost::system::error_code& error) const;
    [[nodiscard]] boost::asio::ip::tcp::endpoint remote_endpoint(boost::system::error_code& error) const;

    void shutdown();

   private:
    boost::asio::ip::tcp::socket socket_;
};

}    // namespace media_server

#endif
