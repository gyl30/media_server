#ifndef MEDIA_NET_UDP_TRANSPORT_H
#define MEDIA_NET_UDP_TRANSPORT_H

#include <memory>
#include <span>
#include <cstddef>
#include <cstdint>
#include <utility>

#include <boost/asio/buffer.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/system/error_code.hpp>

namespace media_server
{

class udp_transport final : public std::enable_shared_from_this<udp_transport>
{
   public:
    explicit udp_transport(boost::asio::io_context& owner);

   public:
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
    template <typename Handler>
    void async_write(std::span<const std::uint8_t> data, const boost::asio::ip::udp::endpoint& endpoint, Handler&& handler)
    {
        socket_.async_send_to(boost::asio::buffer(data), endpoint, std::forward<Handler>(handler));
    }
    [[nodiscard]] boost::asio::ip::udp::endpoint local_endpoint(boost::system::error_code& error) const;
    void shutdown();

   private:
    boost::asio::ip::udp::socket socket_;
};

}    // namespace media_server

#endif
