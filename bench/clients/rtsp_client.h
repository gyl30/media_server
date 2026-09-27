#ifndef MEDIA_SERVER_BENCH_CLIENTS_RTSP_CLIENT_H
#define MEDIA_SERVER_BENCH_CLIENTS_RTSP_CLIENT_H

#include <set>
#include <string>
#include <vector>
#include <cstddef>
#include <cstdint>

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>

struct rtsp_client_t;
struct rtsp_rtp_info_t;

namespace media_server::bench
{

class rtsp_client final
{
   public:
    rtsp_client(boost::asio::io_context& io, std::string path);

    ~rtsp_client();

   public:
    boost::asio::awaitable<boost::system::error_code> play(std::string host, std::uint16_t port);
    boost::asio::awaitable<boost::system::error_code> consume_one();
    void cancel() noexcept;

    [[nodiscard]] std::uint64_t received_bytes() const noexcept { return received_bytes_; }
    [[nodiscard]] std::uint64_t received_messages() const noexcept { return received_messages_; }
    [[nodiscard]] std::uint64_t received_audio_bytes() const noexcept { return received_audio_bytes_; }
    [[nodiscard]] std::uint64_t received_audio_messages() const noexcept { return received_audio_messages_; }

   private:
    static int send_callback(void* param, const char* uri, const void* request, std::size_t bytes);
    static int rtp_port_callback(void* param, int media, const char* source, unsigned short port[2], char* ip, int len);
    static int describe_callback(void* param, const char* sdp, int len);
    static int setup_callback(void* param, int timeout, std::int64_t duration);
    static int play_callback(void* param,
                             int media,
                             const std::uint64_t* nptbegin,
                             const std::uint64_t* nptend,
                             const double* scale,
                             const ::rtsp_rtp_info_t* rtpinfo,
                             int count);
    static int ignore_callback(void* param);
    static void rtp_callback(void* param, std::uint8_t channel, const void* data, std::uint16_t bytes);

   private:
    boost::asio::awaitable<boost::system::error_code> flush();
    boost::asio::awaitable<boost::system::error_code> start(std::string host, std::uint16_t port);

   private:
    boost::asio::ip::tcp::resolver resolver_;
    boost::asio::ip::tcp::socket socket_;
    std::string path_;
    std::string uri_;
    std::string sdp_;
    std::vector<std::vector<std::uint8_t>> writes_;
    std::set<std::uint8_t> received_channels_;
    std::uint64_t received_bytes_{};
    std::uint64_t received_messages_{};
    std::uint64_t received_audio_bytes_{};
    std::uint64_t received_audio_messages_{};
    rtsp_client_t* client_{};
    bool completed_{};
};

}    // namespace media_server::bench

#endif
