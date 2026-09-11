#ifndef MEDIA_SERVER_TESTS_CLIENTS_RTSP_TEST_CLIENT_H
#define MEDIA_SERVER_TESTS_CLIENTS_RTSP_TEST_CLIENT_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

struct rtsp_client_t;
struct rtsp_rtp_info_t;

namespace media_server::test
{

class rtsp_test_client final
{
   public:
    rtsp_test_client(boost::asio::io_context& io, std::string path);
    ~rtsp_test_client();

    boost::asio::awaitable<boost::system::error_code> publish(
        std::string host, std::uint16_t port, std::string sdp, std::vector<std::uint8_t> rtp);
    boost::asio::awaitable<boost::system::error_code> play(std::string host, std::uint16_t port, std::size_t rtp_media_count = 1);

    [[nodiscard]] const std::string& sdp() const noexcept { return sdp_; }
    [[nodiscard]] const std::vector<std::uint8_t>& rtp() const noexcept { return rtp_; }
    [[nodiscard]] const std::map<std::uint8_t, std::vector<std::uint8_t>>& rtp_by_channel() const noexcept { return rtp_by_channel_; }

   private:
    enum class mode
    {
        publish,
        play,
    };

    static int send_callback(void* param, const char* uri, const void* request, std::size_t bytes);
    static int rtp_port_callback(void* param, int media, const char* source, unsigned short port[2], char* ip, int len);
    static int announce_callback(void* param);
    static int describe_callback(void* param, const char* sdp, int len);
    static int setup_callback(void* param, int timeout, std::int64_t duration);
    static int play_callback(void* param,
                             int media,
                             const std::uint64_t* nptbegin,
                             const std::uint64_t* nptend,
                             const double* scale,
                             const ::rtsp_rtp_info_t* rtpinfo,
                             int count);
    static int record_callback(void* param,
                               int media,
                               const std::uint64_t* nptbegin,
                               const std::uint64_t* nptend,
                               const double* scale,
                               const ::rtsp_rtp_info_t* rtpinfo,
                               int count);
    static int ignore_callback(void* param);
    static void rtp_callback(void* param, std::uint8_t channel, const void* data, std::uint16_t bytes);
    boost::asio::awaitable<boost::system::error_code> flush();
    boost::asio::awaitable<boost::system::error_code> start(std::string host, std::uint16_t port, mode operation);

    boost::asio::ip::tcp::resolver resolver_;
    boost::asio::ip::tcp::socket socket_;
    std::string path_;
    std::string uri_;
    std::string sdp_;
    std::vector<std::vector<std::uint8_t>> writes_;
    std::vector<std::uint8_t> rtp_;
    std::map<std::uint8_t, std::vector<std::uint8_t>> rtp_by_channel_;
    rtsp_client_t* client_{};
    mode mode_{mode::publish};
    bool completed_{};
};

}    // namespace media_server::test

#endif
