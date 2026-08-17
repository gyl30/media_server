#ifndef MEDIA_RTSP_RTSP_OUTPUT_SESSION_H
#define MEDIA_RTSP_RTSP_OUTPUT_SESSION_H

#include "media/codec/output_video_config.h"
#include "media/codec/video_transcoder.h"
#include "media/core/media_reader.h"
#include "media/core/stream_registry.h"
#include "media/net/tcp_connection.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

struct rtsp_server_t;
struct rtsp_muxer_t;
struct rtsp_header_transport_t;

namespace media_server
{

class rtsp_output_session final : public media_reader, public std::enable_shared_from_this<rtsp_output_session>
{
   public:
    rtsp_output_session(std::shared_ptr<tcp_connection> connection, stream_registry& registry, std::uint16_t server_port, output_video_config video = {});
    ~rtsp_output_session() override;

    void startup(std::vector<std::uint8_t> initial_data = {});
    void shutdown();

    void on_tracks(media_track_snapshot_ptr tracks) override;
    void on_read(media_read_batch batch) override;
    void on_end() override;

   private:
    struct track_state
    {
        codec_id codec{};
        std::uint64_t config_version{};
        int payload_index{-1};
        int media_id{-1};
        std::uint8_t rtp_channel{};
        std::uint8_t rtcp_channel{};
        bool setup{};
    };

    static int send_callback(void* param, const void* data, std::size_t bytes);
    static int describe_callback(void* param, rtsp_server_t* server, const char* uri);
    static int setup_callback(
        void* param, rtsp_server_t* server, const char* uri, const char* session, const rtsp_header_transport_t transports[], std::size_t count);
    static int play_callback(void* param, rtsp_server_t* server, const char* uri, const char* session, const std::int64_t* npt, const double* scale);
    static int teardown_callback(void* param, rtsp_server_t* server, const char* uri, const char* session);
    static int get_parameter_callback(void* param, rtsp_server_t* server, const char* uri, const char* session, const void* content, int bytes);
    static int muxer_packet_callback(void* param, int pid, const void* data, int bytes, std::uint32_t timestamp, int flags);

    void on_tcp_read(std::span<const std::uint8_t> data);
    void safe_shutdown();
    bool apply_tracks(const media_track_snapshot_ptr& tracks);
    int on_describe(std::string_view uri);
    int on_setup(std::string_view uri, std::string_view session, const rtsp_header_transport_t transports[], std::size_t count);
    int on_play(std::string_view uri, std::string_view session, const std::int64_t* npt);
    int on_muxer_packet(int pid, const void* data, int bytes);
    bool configure_tracks(std::span<const media_track> tracks, std::string& sdp);
    [[nodiscard]] bool description_current() const;
    [[nodiscard]] bool channels_available(track_id id, int rtp_channel, int rtcp_channel) const;
    [[nodiscard]] static std::string stream_name_from_uri(std::string_view uri);
    [[nodiscard]] static std::optional<track_id> track_id_from_uri(std::string_view uri);

    std::shared_ptr<tcp_connection> connection_;
    stream_registry& registry_;
    std::uint16_t server_port_{};
    output_video_config video_config_;
    rtsp_server_t* server_{};
    rtsp_muxer_t* muxer_{};
    std::shared_ptr<media_stream> stream_;
    media_reader_handle reader_;
    std::map<track_id, track_state> tracks_;
    std::unique_ptr<video_transcoder> video_transcoder_;
    track_id video_track_id_{};
    media_reader_cursor reader_cursor_;
    std::uint64_t track_revision_{};
    std::string stream_name_;
    std::string session_id_;
    bool playing_{};
    bool closed_{};
};

}    // namespace media_server

#endif
