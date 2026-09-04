#ifndef MEDIA_NET_UDP_YIELD_TRANSPORT_H
#define MEDIA_NET_UDP_YIELD_TRANSPORT_H

#include <span>
#include <cstddef>
#include <cstdint>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/system/error_code.hpp>

namespace media_server
{

class udp_yield_transport final
{
   public:
    explicit udp_yield_transport(boost::asio::io_context& owner);

    void startup(boost::asio::ip::address bind_address, std::uint16_t port, boost::system::error_code& error);
    void connect(const boost::asio::ip::udp::endpoint& endpoint, boost::system::error_code& error);
    std::size_t read(std::span<std::uint8_t> buffer,
                     boost::asio::ip::udp::endpoint& endpoint,
                     boost::asio::yield_context& yield,
                     boost::system::error_code& error);
    std::size_t write(std::span<const std::uint8_t> data,
                      const boost::asio::ip::udp::endpoint& endpoint,
                      boost::asio::yield_context& yield,
                      boost::system::error_code& error);
    [[nodiscard]] boost::asio::ip::udp::endpoint local_endpoint(boost::system::error_code& error) const;
    void shutdown();

   private:
    boost::asio::ip::udp::socket socket_;
};

}    // namespace media_server

#endif
