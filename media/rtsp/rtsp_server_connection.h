#ifndef MEDIA_RTSP_RTSP_SERVER_CONNECTION_H
#define MEDIA_RTSP_RTSP_SERVER_CONNECTION_H

#include <span>
#include <memory>
#include <string>
#include <cstdint>

#include <boost/asio/any_io_executor.hpp>

#include "media/codec/output_video_config.h"
#include "media/net/tcp_connection.h"

extern "C"
{
#include "rtp-over-rtsp.h"
#include "rtsp-server.h"
}

namespace media_server
{

class rtsp_server_session;

class rtsp_server_connection final : public std::enable_shared_from_this<rtsp_server_connection>
{
   public:
    rtsp_server_connection(std::shared_ptr<tcp_connection> connection, output_video_codec video_codec);
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

    void on_tcp_read(std::span<const std::uint8_t> data);
    void safe_shutdown();

    boost::asio::any_io_executor executor_;
    output_video_codec video_codec_;
    std::shared_ptr<tcp_connection> connection_;
    std::shared_ptr<rtsp_server_session> session_;
    std::string local_address_;
    rtsp_server_t* server_{};
    rtp_over_rtsp_t interleaved_{};
    bool rtsp_need_more_data_{};
    bool closed_{};
};

}    // namespace media_server

#endif
