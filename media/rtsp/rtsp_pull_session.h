#ifndef MEDIA_RTSP_RTSP_PULL_SESSION_H
#define MEDIA_RTSP_RTSP_PULL_SESSION_H

#include <array>
#include <chrono>
#include <memory>
#include <string>
#include <cstdint>
#include <optional>

#include <boost/asio.hpp>

#include "media/net/tcp_connection.h"
#include "media/core/media_stream.h"

extern "C"
{
#include "avpkt2bs.h"
#include "rtsp-client.h"
}

struct rtsp_client_t;
struct rtsp_demuxer_t;
struct avpacket_t;

namespace media_server
{

class rtsp_pull_session final : public std::enable_shared_from_this<rtsp_pull_session>
{
   public:
    rtsp_pull_session(boost::asio::io_context& io,
                      std::string stream_name,
                      std::string url,
                      std::chrono::milliseconds establishment_timeout = std::chrono::milliseconds{15'000},
                      std::chrono::milliseconds initial_tracks_timeout = std::chrono::milliseconds{15'000});
    ~rtsp_pull_session();

    bool startup();
    void shutdown();

   private:
    struct parsed_url
    {
        std::string request_url;
        std::string host;
        std::uint16_t port{554};
        std::string username;
        std::string password;
    };

    static int send_callback(void* param, const char* uri, const void* request, std::size_t bytes);
    static int rtp_port_callback(void* param, int media, const char* source, unsigned short port[2], char* ip, int length);
    static int describe_callback(void* param, const char* sdp, int length);
    static int setup_callback(void* param, int timeout, std::int64_t duration);
    static int play_callback(
        void* param, int media, const std::uint64_t* begin, const std::uint64_t* end, const double* scale, const rtsp_rtp_info_t* info, int count);
    static int pause_callback(void* param);
    static int teardown_callback(void* param);
    static void rtp_callback(void* param, std::uint8_t channel, const void* data, std::uint16_t bytes);
    static int packet_callback(void* param, avpacket_t* packet);

    [[nodiscard]] static std::optional<parsed_url> parse_url(std::string_view url);
    void on_connect(const boost::system::error_code& error, boost::asio::ip::tcp::socket socket);
    void on_read(std::span<const std::uint8_t> data);
    void safe_shutdown();
    void record_establishment_progress();
    void wait_establishment_timeout();
    void wait_keepalive();
    void wait_rtcp();
    int on_describe(const char* sdp, int length);
    int on_setup(int timeout, std::int64_t duration);
    void on_rtp(std::uint8_t channel, const void* data, std::uint16_t bytes);
    int on_packet(avpacket_t* packet);
    [[nodiscard]] bool update_track_from_packet(const avpacket_t& packet);
    [[nodiscard]] bool try_initialize_tracks();

    boost::asio::io_context& io_;
    std::string stream_name_;
    std::string url_;
    std::string username_;
    std::string password_;
    boost::asio::ip::tcp::resolver resolver_;
    boost::asio::ip::tcp::socket connect_socket_;
    boost::asio::steady_timer establishment_timer_;
    boost::asio::steady_timer initial_tracks_timer_;
    boost::asio::steady_timer keepalive_timer_;
    boost::asio::steady_timer rtcp_timer_;
    std::shared_ptr<tcp_connection> connection_;
    std::shared_ptr<media_stream> stream_;
    rtsp_client_t* client_{};
    std::array<rtsp_demuxer_t*, 2> demuxers_{};
    avpkt2bs_t bitstream_{};
    std::chrono::milliseconds establishment_timeout_;
    std::chrono::milliseconds initial_tracks_timeout_;
    std::chrono::steady_clock::time_point last_establishment_progress_{};
    std::chrono::seconds keepalive_interval_{30};
    std::optional<media_track> initial_video_track_;
    std::optional<media_track> initial_audio_track_;
    bool expected_audio_{};
    bool tracks_initialized_{};
    bool media_started_{};
    bool closed_{};
};

}    // namespace media_server

#endif
