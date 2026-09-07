#ifndef MEDIA_RTSP_RTSP_SERVER_CONNECTION_H
#define MEDIA_RTSP_RTSP_SERVER_CONNECTION_H

#include <span>
#include <deque>
#include <chrono>
#include <memory>
#include <vector>
#include <cstdint>

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/steady_timer.hpp>

#include "media/net/tcp_yield_transport.h"
#include "media/codec/video_transcode_config.h"

extern "C"
{
#include "rtsp-server.h"
}

namespace media_server
{

class worker_context;
class rtsp_publish_session;
class rtsp_play_session;

class rtsp_server_connection final : public std::enable_shared_from_this<rtsp_server_connection>
{
   public:
    rtsp_server_connection(worker_context& worker,
                           boost::asio::ip::tcp::socket socket,
                           video_transcode_codec video_codec,
                           std::chrono::milliseconds inactivity_timeout = std::chrono::milliseconds{60'000},
                           std::size_t max_write_queue_bytes = 1024U * 1024U);
    ~rtsp_server_connection();

    void startup();
    void shutdown();

   private:
    static int send_callback(void* param, const void* data, std::size_t bytes);
    static void interleaved_callback(void* param, std::uint8_t channel, const void* data, std::uint16_t bytes);
    static int describe_callback(void* param, rtsp_server_t* server, const char* uri);
    static int setup_callback(
        void* param, rtsp_server_t* server, const char* uri, const char* session, const rtsp_header_transport_t transports[], std::size_t count);
    static int play_callback(void* param, rtsp_server_t* server, const char* uri, const char* session, const std::int64_t* npt, const double* scale);
    static int teardown_callback(void* param, rtsp_server_t* server, const char* uri, const char* session);
    static int announce_callback(void* param, rtsp_server_t* server, const char* uri, const char* sdp, int length);
    static int record_callback(
        void* param, rtsp_server_t* server, const char* uri, const char* session, const std::int64_t* npt, const double* scale);
    static int options_callback(void* param, rtsp_server_t* server, const char* uri);
    static int get_parameter_callback(void* param, rtsp_server_t* server, const char* uri, const char* session, const void* content, int bytes);

    void run(boost::asio::yield_context yield);
    void run_write(boost::asio::yield_context yield);
    void write(std::span<const std::uint8_t> data);
    void safe_shutdown();
    void record_control_activity();
    void schedule_inactivity_timeout();

    worker_context& worker_;
    video_transcode_codec video_codec_;
    tcp_yield_transport transport_;
    boost::asio::steady_timer inactivity_timer_;
    std::chrono::milliseconds inactivity_timeout_;
    std::chrono::steady_clock::time_point last_control_activity_{};
    std::size_t max_write_queue_bytes_;
    std::size_t queued_write_bytes_{};
    std::deque<std::shared_ptr<std::vector<std::uint8_t>>> write_queue_;
    std::shared_ptr<rtsp_publish_session> publish_session_;
    std::shared_ptr<rtsp_play_session> play_session_;
    boost::asio::ip::address local_address_;
};

}    // namespace media_server

#endif
