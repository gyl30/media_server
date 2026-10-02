#ifndef MEDIA_NET_UDP_TRANSPORT_H
#define MEDIA_NET_UDP_TRANSPORT_H

#include <deque>
#include <functional>
#include <memory>
#include <vector>
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
    using write_callback = std::function<void(boost::system::error_code, std::size_t)>;

    explicit udp_transport(boost::asio::io_context& owner);

   public:
    void startup(boost::asio::ip::address bind_address, std::uint16_t port, boost::system::error_code& error);
    void connect(const boost::asio::ip::udp::endpoint& endpoint, boost::system::error_code& error);
    std::size_t read(std::span<std::uint8_t> buffer,
                     boost::asio::ip::udp::endpoint& endpoint,
                     boost::asio::yield_context& yield,
                     boost::system::error_code& error);
    // 与 startup/connect/read 一样，写入和关闭由 owner executor 调用。
    // 返回入队结果；overflow 丢弃新包并返回 false，callback 只报告实际发送的 completion。
    void set_write_callback(write_callback callback);
    bool write(std::span<const std::uint8_t> data, boost::asio::ip::udp::endpoint endpoint);
    bool write(std::vector<std::uint8_t> data, boost::asio::ip::udp::endpoint endpoint);
    [[nodiscard]] boost::asio::ip::udp::endpoint local_endpoint(boost::system::error_code& error) const;
    void shutdown();

   private:
    struct pending_datagram
    {
        std::vector<std::uint8_t> packet;
        boost::asio::ip::udp::endpoint endpoint;
    };

    void start_write();
    void on_write(const std::shared_ptr<pending_datagram>& datagram, boost::system::error_code error, std::size_t bytes);

    boost::asio::ip::udp::socket socket_;
    std::deque<std::shared_ptr<pending_datagram>> write_queue_;
    std::size_t queued_write_bytes_{};
    std::shared_ptr<write_callback> write_callback_;
    bool stopped_{};
};

}    // namespace media_server

#endif
