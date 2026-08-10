#ifndef MEDIA_RTMP_RTMP_SESSION_H
#define MEDIA_RTMP_RTMP_SESSION_H

#include "media/core/media_sink.h"
#include "media/core/stream_registry.h"
#include "media/net/tcp_connection.h"
#include "media/rtmp/rtmp_timestamp.h"
#include "media/flv/flv_output_muxer.h"

#include <cstdint>
#include <memory>
#include <string>

struct flv_demuxer_t;
struct rtmp_server_t;

namespace media_server
{

class rtmp_session final : public media_sink, public std::enable_shared_from_this<rtmp_session>
{
   public:
    rtmp_session(std::shared_ptr<tcp_connection> connection, stream_registry& registry);
    ~rtmp_session() override;

    void start();
    void shutdown();

    void on_track(const media_track& track) override;
    void on_frame(const media_frame& frame) override;
    void on_end() override;

   private:
    enum class role
    {
        none,
        publisher,
        player,
    };

    static int send_callback(void* param, const void* header, std::size_t header_bytes, const void* payload, std::size_t payload_bytes);
    static int play_callback(void* param, const char* app, const char* stream, double start, double duration, std::uint8_t reset);
    static int pause_callback(void* param, int pause, std::uint32_t milliseconds);
    static int seek_callback(void* param, std::uint32_t milliseconds);
    static int publish_callback(void* param, const char* app, const char* stream, const char* type);
    static int video_callback(void* param, const void* data, std::size_t bytes, std::uint32_t timestamp);
    static int audio_callback(void* param, const void* data, std::size_t bytes, std::uint32_t timestamp);
    static int script_callback(void* param, const void* data, std::size_t bytes, std::uint32_t timestamp);
    static int duration_callback(void* param, const char* app, const char* stream, double* duration);
    static int demux_callback(void* param, int codec, const void* data, std::size_t bytes, std::uint32_t pts, std::uint32_t dts, int flags);

    int on_play(std::string app, std::string stream);
    int on_publish(std::string app, std::string stream);
    int on_flv_demux(int codec, std::span<const std::uint8_t> data, std::uint32_t pts, std::uint32_t dts, int flags);
    void on_read(std::span<const std::uint8_t> data);
    void safe_shutdown();
    [[nodiscard]] static std::string make_stream_name(std::string_view app, std::string_view stream);

    std::shared_ptr<tcp_connection> connection_;
    stream_registry& registry_;
    rtmp_server_t* server_{};
    flv_demuxer_t* demuxer_{};
    std::unique_ptr<flv_output_muxer> output_muxer_;
    rtmp_timestamp_state video_timestamp_;
    rtmp_timestamp_state audio_timestamp_;
    std::shared_ptr<media_stream> stream_;
    std::string stream_name_;
    role role_{role::none};
    bool closed_{};
};

}    // namespace media_server

#endif
