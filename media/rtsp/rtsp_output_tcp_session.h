#ifndef MEDIA_RTSP_RTSP_OUTPUT_TCP_SESSION_H
#define MEDIA_RTSP_RTSP_OUTPUT_TCP_SESSION_H

#include "media/codec/video_transcoder.h"
#include "media/core/media_reader.h"
#include "media/core/stream_registry.h"
#include "media/rtsp/rtsp_output_track.h"
#include "media/rtsp/rtsp_server_connection.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct rtsp_muxer_t;
struct rtsp_server_t;
struct rtsp_header_transport_t;

namespace media_server
{

class rtsp_output_tcp_session final : public media_reader, public std::enable_shared_from_this<rtsp_output_tcp_session>
{
   public:
    rtsp_output_tcp_session(std::weak_ptr<rtsp_server_connection> connection,
                            stream_registry& registry,
                            std::shared_ptr<media_stream> stream,
                            std::string stream_name,
                            std::vector<rtsp_output_track_description> tracks,
                            std::shared_ptr<video_transcoder> video_transcoder,
                            track_id video_track_id);
    ~rtsp_output_tcp_session() override;

    int startup(rtsp_server_t* server,
                std::string_view uri,
                std::string_view session,
                const rtsp_header_transport_t transports[],
                std::size_t count);
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

    static int muxer_packet_callback(void* param, int pid, const void* data, int bytes, std::uint32_t timestamp, int flags);

    [[nodiscard]] std::size_t on_control_read(std::span<const std::uint8_t> data);
    void safe_shutdown();
    bool create_muxer();
    bool apply_tracks(const media_track_snapshot_ptr& tracks);
    int on_setup(rtsp_server_t* server,
                 std::string_view uri,
                 std::string_view session,
                 const rtsp_header_transport_t transports[],
                 std::size_t count);
    int on_play(rtsp_server_t* server, std::string_view uri, std::string_view session, const std::int64_t* npt);
    int on_teardown(rtsp_server_t* server, std::string_view session);
    int on_muxer_packet(int pid, const void* data, int bytes);
    [[nodiscard]] bool description_current() const;
    [[nodiscard]] bool channels_available(track_id id, int rtp_channel, int rtcp_channel) const;
    [[nodiscard]] static std::string stream_name_from_uri(std::string_view uri);
    [[nodiscard]] static std::optional<track_id> track_id_from_uri(std::string_view uri);

    std::weak_ptr<rtsp_server_connection> connection_;
    stream_registry& registry_;
    std::shared_ptr<media_stream> stream_;
    std::string stream_name_;
    std::vector<rtsp_output_track_description> descriptions_;
    rtsp_muxer_t* muxer_{};
    media_reader_handle reader_;
    std::map<track_id, track_state> tracks_;
    std::shared_ptr<video_transcoder> video_transcoder_;
    track_id video_track_id_{};
    media_reader_cursor reader_cursor_;
    std::uint64_t track_revision_{};
    std::string session_id_;
    bool playing_{};
    bool closed_{};
};

}    // namespace media_server

#endif
