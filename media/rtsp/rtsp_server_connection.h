#ifndef MEDIA_RTSP_RTSP_SERVER_CONNECTION_H
#define MEDIA_RTSP_RTSP_SERVER_CONNECTION_H

#include <span>
#include <memory>
#include <string>
#include <cstdint>
#include <functional>

#include <boost/asio/any_io_executor.hpp>

#include "media/net/tcp_connection.h"

extern "C"
{
#include "rtp-over-rtsp.h"
#include "rtsp-server.h"
}

namespace media_server
{

struct rtsp_server_connection_handler
{
    std::function<void(std::uint8_t, std::span<const std::uint8_t>)> on_interleaved;
    std::function<void()> on_shutdown;
    std::function<int(rtsp_server_t*, const char*)> on_describe;
    std::function<int(rtsp_server_t*, const char*, const char*, const rtsp_header_transport_t[], std::size_t)> on_setup;
    std::function<int(rtsp_server_t*, const char*, const char*, const std::int64_t*, const double*)> on_play;
    std::function<int(rtsp_server_t*, const char*, const char*)> on_teardown;
    std::function<int(rtsp_server_t*, const char*, const char*, int)> on_announce;
    std::function<int(rtsp_server_t*, const char*, const char*, const std::int64_t*, const double*)> on_record;
    std::function<int(rtsp_server_t*, const char*, const char*, const void*, int)> on_get_parameter;
};

class rtsp_server_connection final : public std::enable_shared_from_this<rtsp_server_connection>
{
   public:
    explicit rtsp_server_connection(std::shared_ptr<tcp_connection> connection);
    ~rtsp_server_connection();

    bool startup(std::shared_ptr<const rtsp_server_connection_handler> handler);
    void set_handler(std::shared_ptr<const rtsp_server_connection_handler> handler);
    void write(std::span<const std::uint8_t> data);
    void shutdown();

    [[nodiscard]] std::string local_address() const;

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

    [[nodiscard]] std::size_t input(std::span<const std::uint8_t> data);
    void on_tcp_read(std::span<const std::uint8_t> data);
    void safe_shutdown();

    boost::asio::any_io_executor executor_;
    std::shared_ptr<tcp_connection> connection_;
    std::shared_ptr<const rtsp_server_connection_handler> handler_;
    std::string local_address_;
    rtsp_server_t* server_{};
    rtp_over_rtsp_t interleaved_{};
    bool closed_{};
};

}    // namespace media_server

#endif
