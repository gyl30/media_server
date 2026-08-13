#ifndef MEDIA_NET_TCP_CONNECTION_H
#define MEDIA_NET_TCP_CONNECTION_H

#include <boost/asio.hpp>

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace media_server
{

class tcp_connection final : public std::enable_shared_from_this<tcp_connection>
{
   public:
    using read_handler = std::function<void(boost::system::error_code, std::span<const std::uint8_t>)>;
    using write_error_handler = std::function<void()>;

    explicit tcp_connection(boost::asio::ip::tcp::socket socket);

    void startup(read_handler on_read, write_error_handler on_write_error);
    void write(std::span<const std::uint8_t> data);
    void write(const void* data, std::size_t bytes);
    void shutdown();

    [[nodiscard]] boost::asio::ip::tcp::socket& socket() noexcept;

   private:
    void read_next();
    void write_next();
    void safe_shutdown();

    boost::asio::ip::tcp::socket socket_;
    std::array<std::uint8_t, 64 * 1024> read_buffer_{};
    std::deque<std::shared_ptr<std::vector<std::uint8_t>>> write_queue_;
    read_handler on_read_;
    write_error_handler on_write_error_;
    bool closed_{};
};

}    // namespace media_server

#endif
