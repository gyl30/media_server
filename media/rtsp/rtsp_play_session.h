#ifndef MEDIA_RTSP_RTSP_PLAY_SESSION_H
#define MEDIA_RTSP_RTSP_PLAY_SESSION_H

#include <map>
#include <span>
#include <memory>
#include <string>
#include <cstdint>
#include <utility>
#include <functional>
#include <string_view>

#include <boost/asio/ip/address.hpp>
#include <boost/system/error_code.hpp>

#include "media/core/media_reader.h"
#include "media/codec/video_transcoder.h"
#include "media/codec/video_transcode_config.h"

struct rtsp_muxer_t;
struct rtsp_server_t;
struct rtsp_header_transport_t;

namespace media_server
{

class worker_context;

class rtsp_play_session final : public media_reader, public std::enable_shared_from_this<rtsp_play_session>
{
   public:
    rtsp_play_session(worker_context& worker,
                        video_transcode_codec video_codec,
                        boost::asio::ip::address local_address,
                        std::function<void(std::span<const std::uint8_t>)> write);

    void set_error_handler(std::function<void(boost::system::error_code)> handler) { error_handler_ = std::move(handler); }

    void on_interleaved(std::uint8_t channel, std::span<const std::uint8_t> data);
    int on_describe(rtsp_server_t* server, std::string_view uri);
    int on_setup(rtsp_server_t* server,
                 std::string_view uri,
                 std::string_view session,
                 const rtsp_header_transport_t transports[],
                 std::size_t count);
    int on_play(rtsp_server_t* server, std::string_view uri, std::string_view session, const std::int64_t* npt, const double* scale);
    int on_teardown(rtsp_server_t* server, std::string_view uri, std::string_view session);
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
        int rtp_channel{-1};
        int rtcp_channel{-1};
    };

    static int muxer_packet_callback(void* param, int pid, const void* data, int bytes, std::uint32_t timestamp, int flags);
    void safe_shutdown();
    [[nodiscard]] int prepare_presentation(std::string_view uri);
    [[nodiscard]] bool apply_tracks(const media_track_snapshot_ptr& tracks);
    int on_muxer_packet(int pid, const void* data, int bytes);
    [[nodiscard]] int presentation_status() const;
    [[nodiscard]] bool channels_available(track_id id, int rtp_channel, int rtcp_channel) const;

    worker_context& worker_;
    video_transcode_codec video_codec_;
    boost::asio::ip::address local_address_;
    std::function<void(std::span<const std::uint8_t>)> write_handler_;
    std::function<void(boost::system::error_code)> error_handler_;
    std::shared_ptr<media_stream> stream_;
    std::map<track_id, track_state> track_states_;
    std::unique_ptr<video_transcoder> video_transcoder_;
    rtsp_muxer_t* muxer_{};
    track_id video_track_id_{};
    media_reader_cursor reader_cursor_;
    std::uint64_t track_revision_{};
    std::string session_id_;
    bool playing_{};
    bool closed_{};
};

}    // namespace media_server

#endif
