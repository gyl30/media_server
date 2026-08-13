#ifndef MEDIA_NET_UDP_SOCKET_H
#define MEDIA_NET_UDP_SOCKET_H

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/udp.hpp>

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace media_server
{

class udp_socket final : public std::enable_shared_from_this<udp_socket>
{
   public:
    using read_handler =
        std::function<void(boost::system::error_code, std::span<const std::uint8_t>, const boost::asio::ip::udp::endpoint&)>;
    using write_error_handler = std::function<void(boost::system::error_code, const boost::asio::ip::udp::endpoint&)>;

    explicit udp_socket(boost::asio::any_io_executor executor);

    [[nodiscard]] bool startup(boost::asio::ip::address bind_address, read_handler on_read, write_error_handler on_write_error);
    void send(std::vector<std::uint8_t> packet, boost::asio::ip::udp::endpoint endpoint);
    void shutdown();

    [[nodiscard]] std::uint16_t local_port() const noexcept;

   private:
    struct pending_datagram
    {
        std::shared_ptr<std::vector<std::uint8_t>> packet;
        boost::asio::ip::udp::endpoint endpoint;
    };

    void receive_next();
    void write_next();
    void safe_shutdown();

    boost::asio::ip::udp::socket socket_;
    std::array<std::uint8_t, 64 * 1024> receive_buffer_{};
    boost::asio::ip::udp::endpoint receive_endpoint_;
    std::deque<pending_datagram> send_queue_;
    read_handler on_read_;
    write_error_handler on_write_error_;
    std::uint16_t local_port_{};
    bool closed_{};
};

}    // namespace media_server

#endif
