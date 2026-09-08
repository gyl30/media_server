#ifndef MEDIA_SERVER_TESTS_CLIENTS_RTMP_TEST_CLIENT_H
#define MEDIA_SERVER_TESTS_CLIENTS_RTMP_TEST_CLIENT_H

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/ip/tcp.hpp>

struct rtmp_client_t;

namespace media_server::test
{

class rtmp_test_client final
{
   public:
    rtmp_test_client(boost::asio::any_io_executor executor, std::string app, std::string stream);
    ~rtmp_test_client();

    boost::asio::awaitable<boost::system::error_code> publish(
        std::string host, std::uint16_t port, std::vector<std::uint8_t> metadata, std::vector<std::uint8_t> video_config);
    boost::asio::awaitable<boost::system::error_code> play(std::string host, std::uint16_t port);

    [[nodiscard]] const std::vector<std::uint8_t>& video() const noexcept { return video_; }

   private:
    static int send_callback(void* param, const void* header, std::size_t header_bytes, const void* payload, std::size_t payload_bytes);
    static int video_callback(void* param, const void* data, std::size_t bytes, std::uint32_t timestamp);
    static int ignore_callback(void* param, const void* data, std::size_t bytes, std::uint32_t timestamp);
    boost::asio::awaitable<boost::system::error_code> flush();

    boost::asio::ip::tcp::resolver resolver_;
    boost::asio::ip::tcp::socket socket_;
    std::string app_;
    std::string stream_;
    std::vector<std::vector<std::uint8_t>> writes_;
    std::vector<std::uint8_t> video_;
    rtmp_client_t* client_{};
};

}    // namespace media_server::test

#endif
