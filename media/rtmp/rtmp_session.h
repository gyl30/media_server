#ifndef MEDIA_RTMP_RTMP_SESSION_H
#define MEDIA_RTMP_RTMP_SESSION_H

#include "media/core/media_reader.h"
#include "media/core/stream_registry.h"
#include "media/net/tcp_connection.h"
#include "media/rtmp/rtmp_timestamp.h"
#include "media/flv/flv_output_muxer.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

struct flv_demuxer_t;
struct rtmp_server_t;

namespace media_server
{

class rtmp_session final : public media_reader, public std::enable_shared_from_this<rtmp_session>
{
   public:
    rtmp_session(std::shared_ptr<tcp_connection> connection, stream_registry& registry);
    ~rtmp_session() override;

    void startup();
    void shutdown();

    void on_tracks(media_track_snapshot_ptr tracks) override;
    void on_read(media_read_batch batch) override;
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
    void on_tcp_read(boost::system::error_code error, std::span<const std::uint8_t> data);
    void on_tcp_write(boost::system::error_code error, std::size_t write_size);
    void safe_shutdown();
    void apply_tracks(const media_track_snapshot_ptr& tracks);
    [[nodiscard]] static std::string make_stream_name(std::string_view app, std::string_view stream);

    std::shared_ptr<tcp_connection> connection_;
    stream_registry& registry_;
    rtmp_server_t* server_{};
    flv_demuxer_t* demuxer_{};
    std::unique_ptr<flv_output_muxer> output_muxer_;
    media_reader_handle reader_;
    std::map<track_id, media_track> reader_tracks_;
    rtmp_timestamp_state timestamp_;
    std::shared_ptr<media_stream> stream_;
    std::string stream_name_;
    role role_{role::none};
    media_reader_cursor reader_cursor_;
    std::uint64_t track_revision_{};
    bool waiting_for_key_frame_{};
    bool closed_{};
};

}    // namespace media_server

#endif
