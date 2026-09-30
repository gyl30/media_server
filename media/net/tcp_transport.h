#ifndef MEDIA_NET_TCP_TRANSPORT_H
#define MEDIA_NET_TCP_TRANSPORT_H

#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <vector>
#include <cstddef>
#include <cstdint>

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/system/error_code.hpp>

namespace media_server
{

class tcp_transport final : public std::enable_shared_from_this<tcp_transport>
{
   public:
    using write_callback = std::function<void(boost::system::error_code, std::size_t)>;

    explicit tcp_transport(boost::asio::ip::tcp::socket socket, std::size_t max_write_queue_bytes = 1024U * 1024U);

   public:
    void set_write_callback(write_callback callback);
    std::size_t read(std::span<std::uint8_t> buffer, boost::asio::yield_context& yield, boost::system::error_code& error);
    void write(std::span<const std::uint8_t> data);
    void write(std::vector<std::uint8_t> data);

    [[nodiscard]] boost::asio::ip::tcp::endpoint local_endpoint(boost::system::error_code& error) const;
    [[nodiscard]] boost::asio::ip::tcp::endpoint remote_endpoint(boost::system::error_code& error) const;

    void shutdown();

   private:
    using buffer = std::shared_ptr<std::vector<std::uint8_t>>;

    void safe_write();
    void on_write(boost::system::error_code error, std::size_t bytes);

   private:
    boost::asio::ip::tcp::socket socket_;
    std::size_t max_write_queue_bytes_;
    std::size_t queued_write_bytes_{};
    std::deque<buffer> write_queue_;
    write_callback write_callback_;
    bool stopped_{};
};

}    // namespace media_server

#endif
