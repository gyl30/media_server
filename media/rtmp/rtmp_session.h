#ifndef MEDIA_RTMP_RTMP_SESSION_H
#define MEDIA_RTMP_RTMP_SESSION_H

#include <memory>
#include <string>
#include <cstdint>
#include <optional>
#include <string_view>

#include <boost/asio/spawn.hpp>
#include <boost/asio/ip/tcp.hpp>

#include "media/net/tcp_transport.h"

struct rtmp_server_t;

namespace media_server
{

[[nodiscard]] std::optional<std::string> parse_rtmp_target(std::string_view app, std::string_view stream);

class worker_context;
class rtmp_publish_session;
class rtmp_play_session;

class rtmp_session final : public std::enable_shared_from_this<rtmp_session>
{
   public:
    rtmp_session(worker_context& worker, boost::asio::ip::tcp::socket socket);
    ~rtmp_session();

   public:
    void startup();
    void shutdown();

   private:
    static int send_callback(void* param, const void* header, std::size_t header_bytes, const void* payload, std::size_t payload_bytes);
    static int delete_stream_callback(void* param, std::uint32_t stream_id);
    static int play_callback(void* param, const char* app, const char* stream, double start, double duration, std::uint8_t reset);
    static int pause_callback(void* param, int pause, std::uint32_t milliseconds);
    static int seek_callback(void* param, std::uint32_t milliseconds);
    static int publish_callback(void* param, const char* app, const char* stream, const char* type);
    static int video_callback(void* param, const void* data, std::size_t bytes, std::uint32_t timestamp);
    static int audio_callback(void* param, const void* data, std::size_t bytes, std::uint32_t timestamp);
    static int script_callback(void* param, const void* data, std::size_t bytes, std::uint32_t timestamp);
    static int duration_callback(void* param, const char* app, const char* stream, double* duration);

   private:
    void run(boost::asio::yield_context yield);
    void run_read(rtmp_server_t* context, boost::asio::yield_context yield);

   private:
    int on_play(std::string_view app, std::string_view stream);
    int on_publish(std::string_view app, std::string_view stream);

   private:
    void safe_shutdown();

   private:
    worker_context& worker_;
    std::shared_ptr<tcp_transport> transport_;
    rtmp_server_t* rtmp_context_{};
    std::unique_ptr<rtmp_publish_session> publish_;
    std::shared_ptr<rtmp_play_session> play_;
};

}    // namespace media_server

#endif
