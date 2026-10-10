#ifndef MEDIA_RTSP_RTSP_PULL_SESSION_H
#define MEDIA_RTSP_RTSP_PULL_SESSION_H

#include <span>
#include <memory>
#include <string>
#include <vector>
#include <cstddef>
#include <cstdint>

#include <boost/asio.hpp>

#include "media/net/idle_timer.h"
#include "media/core/session_registry.h"
#include "media/net/tcp_transport.h"

extern "C"
{
#include "rtsp-client.h"
}

struct rtsp_client_t;

namespace media_server
{

class worker_context;
class rtsp_pull_media;

struct rtsp_pull_config
{
    std::string stream_id;
    std::string url;
    std::string host;
    std::uint16_t port{554};
    std::string username;
    std::string password;
};

class rtsp_pull_session final : public session, public std::enable_shared_from_this<rtsp_pull_session>
{
   public:
    rtsp_pull_session(worker_context& worker, rtsp_pull_config config);
    ~rtsp_pull_session();

   public:
    void startup();
    void shutdown();

   private:
    static int send_callback(void* param, const char* uri, const void* request, std::size_t bytes);
    static int rtp_port_callback(void* param, int media, const char* source, unsigned short port[2], char* ip, int length);
    static int describe_callback(void* param, const char* sdp, int length);
    static int setup_callback(void* param, int, std::int64_t);
    static int play_callback(
        void* param, int media, const std::uint64_t* begin, const std::uint64_t* end, const double* scale, const rtsp_rtp_info_t* info, int count);
    static int pause_callback(void* param);
    static int teardown_callback(void* param);
    static void rtp_callback(void* param, std::uint8_t channel, const void* data, std::uint16_t bytes);

   private:
    void run(boost::asio::yield_context yield);
    void run_read(rtsp_client_t* client, boost::asio::yield_context yield);
    void schedule_rtcp();

   private:
    int on_describe(const char* sdp, int length);
    int on_setup();
    void on_rtp(std::uint8_t channel, const void* data, std::uint16_t bytes);

   private:
    void safe_shutdown();

   private:
    worker_context& worker_;
    rtsp_pull_config config_;
    boost::asio::ip::tcp::resolver resolver_;
    boost::asio::ip::tcp::socket connect_socket_;
    boost::asio::steady_timer rtcp_timer_;
    idle_timer idle_timer_;
    std::shared_ptr<tcp_transport> transport_;
    std::unique_ptr<rtsp_pull_media> media_;
    rtsp_client_t* client_{};
};

}    // namespace media_server

#endif
