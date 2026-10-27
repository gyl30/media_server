#ifndef MEDIA_NET_UDP_TRANSPORT_H
#define MEDIA_NET_UDP_TRANSPORT_H

#include <span>
#include <deque>
#include <memory>
#include <vector>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <functional>

#include <boost/asio/spawn.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/system/error_code.hpp>

namespace media_server
{

enum class udp_receive_buffer
{
    system_default,
    media_input,
};

class udp_transport final : public std::enable_shared_from_this<udp_transport>
{
   public:
    using error_callback = std::function<void(boost::system::error_code)>;

    explicit udp_transport(boost::asio::io_context& owner);

   public:
    void startup(boost::asio::ip::address bind_address, std::uint16_t port, udp_receive_buffer receive_buffer, boost::system::error_code& error);
    void connect(const boost::asio::ip::udp::endpoint& endpoint, boost::system::error_code& error);
    std::size_t read(std::span<std::uint8_t> buffer,
                     boost::asio::ip::udp::endpoint& endpoint,
                     boost::asio::yield_context& yield,
                     boost::system::error_code& error);
    void set_error_callback(error_callback callback);
    bool write(std::span<const std::uint8_t> data, const boost::asio::ip::udp::endpoint& endpoint);
    void shutdown();

   private:
    struct pending_datagram
    {
        std::vector<std::uint8_t> packet;
        boost::asio::ip::udp::endpoint endpoint;
    };

    void start_write();
    void on_write(const std::shared_ptr<pending_datagram>& datagram, boost::system::error_code error);

    boost::asio::ip::udp::socket socket_;
    std::deque<std::shared_ptr<pending_datagram>> write_queue_;
    std::size_t queued_write_bytes_{};
    error_callback error_callback_;
};

}    // namespace media_server

#endif
