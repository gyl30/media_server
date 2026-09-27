#ifndef MEDIA_SERVER_BENCH_CLIENTS_RTMP_CLIENT_H
#define MEDIA_SERVER_BENCH_CLIENTS_RTMP_CLIENT_H

#include <string>
#include <vector>
#include <cstddef>
#include <cstdint>

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>

struct rtmp_client_t;

namespace media_server::bench
{

class rtmp_client final
{
   public:
    rtmp_client(boost::asio::io_context& io, std::string app, std::string stream);

    ~rtmp_client();

   public:
    boost::asio::awaitable<boost::system::error_code> play(std::string host, std::uint16_t port);
    boost::asio::awaitable<boost::system::error_code> consume_one();
    void cancel() noexcept;

    [[nodiscard]] std::uint64_t received_bytes() const noexcept { return received_bytes_; }
    [[nodiscard]] std::uint64_t received_messages() const noexcept { return received_messages_; }
    [[nodiscard]] std::uint64_t received_audio_bytes() const noexcept { return received_audio_bytes_; }
    [[nodiscard]] std::uint64_t received_audio_messages() const noexcept { return received_audio_messages_; }

   private:
    static int send_callback(void* param, const void* header, std::size_t header_bytes, const void* payload, std::size_t payload_bytes);
    static int video_callback(void* param, const void* data, std::size_t bytes, std::uint32_t timestamp);
    static int audio_callback(void* param, const void* data, std::size_t bytes, std::uint32_t timestamp);
    static int ignore_callback(void* param, const void* data, std::size_t bytes, std::uint32_t timestamp);

   private:
    boost::asio::awaitable<boost::system::error_code> flush();

   private:
    boost::asio::ip::tcp::resolver resolver_;
    boost::asio::ip::tcp::socket socket_;
    std::string app_;
    std::string stream_;
    std::vector<std::vector<std::uint8_t>> writes_;
    std::uint64_t received_bytes_{};
    std::uint64_t received_messages_{};
    std::uint64_t received_audio_bytes_{};
    std::uint64_t received_audio_messages_{};
    rtmp_client_t* client_{};
};

}    // namespace media_server::bench

#endif
