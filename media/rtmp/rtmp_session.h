#ifndef MEDIA_RTMP_RTMP_SESSION_H
#define MEDIA_RTMP_RTMP_SESSION_H

#include <chrono>
#include <memory>
#include <string>
#include <cstdint>
#include <string_view>

#include "media/net/tcp_connection.h"
#include "media/core/stream_registry.h"
#include "media/codec/output_video_config.h"

struct rtmp_server_t;

namespace media_server
{

class rtmp_input_session;
class rtmp_output_session;

class rtmp_session final : public std::enable_shared_from_this<rtmp_session>
{
   public:
    rtmp_session(std::shared_ptr<tcp_connection> connection,
                 stream_registry& registry,
                 output_video_config video = {},
                 std::chrono::milliseconds initial_tracks_timeout = std::chrono::milliseconds{15'000});
    ~rtmp_session();

    void startup();
    void shutdown();

   private:
    static int send_callback(void* param, const void* header, std::size_t header_bytes, const void* payload, std::size_t payload_bytes);
    static int play_callback(void* param, const char* app, const char* stream, double start, double duration, std::uint8_t reset);
    static int pause_callback(void* param, int pause, std::uint32_t milliseconds);
    static int seek_callback(void* param, std::uint32_t milliseconds);
    static int publish_callback(void* param, const char* app, const char* stream, const char* type);
    static int video_callback(void* param, const void* data, std::size_t bytes, std::uint32_t timestamp);
    static int audio_callback(void* param, const void* data, std::size_t bytes, std::uint32_t timestamp);
    static int script_callback(void* param, const void* data, std::size_t bytes, std::uint32_t timestamp);
    static int duration_callback(void* param, const char* app, const char* stream, double* duration);

    int on_play(std::string app, std::string stream);
    int on_publish(std::string app, std::string stream);
    void on_tcp_read(boost::system::error_code error, std::span<const std::uint8_t> data);
    void on_tcp_write(boost::system::error_code error, std::size_t write_size);
    void safe_shutdown();
    [[nodiscard]] static std::string make_stream_name(std::string_view app, std::string_view stream);

    std::shared_ptr<tcp_connection> connection_;
    stream_registry& registry_;
    std::chrono::milliseconds initial_tracks_timeout_;
    output_video_config video_config_;
    rtmp_server_t* server_{};
    std::shared_ptr<rtmp_input_session> input_;
    std::shared_ptr<rtmp_output_session> output_;
    std::string stream_name_;
    bool closed_{};
};

}    // namespace media_server

#endif
