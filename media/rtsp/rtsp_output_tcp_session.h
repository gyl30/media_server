#ifndef MEDIA_RTSP_RTSP_OUTPUT_TCP_SESSION_H
#define MEDIA_RTSP_RTSP_OUTPUT_TCP_SESSION_H

#include <map>
#include <span>
#include <memory>
#include <string>
#include <vector>
#include <cstdint>
#include <functional>
#include <utility>

#include <boost/system/error_code.hpp>

#include <boost/asio/any_io_executor.hpp>

#include "media/core/media_reader.h"
#include "media/codec/video_transcoder.h"
#include "media/rtsp/rtsp_output_track.h"

struct rtsp_muxer_t;
struct rtsp_server_t;
struct rtsp_header_transport_t;

namespace media_server
{

class rtsp_output_session;

class rtsp_output_tcp_session final : public media_reader, public std::enable_shared_from_this<rtsp_output_tcp_session>
{
   public:
    rtsp_output_tcp_session(boost::asio::any_io_executor executor,
                            std::shared_ptr<media_stream> stream,
                            std::vector<rtsp_output_track_description> descriptions,
                            track_id video_track_id,
                            std::function<void(std::span<const std::uint8_t>)> write);
    ~rtsp_output_tcp_session() override;

    void on_tracks(media_track_snapshot_ptr tracks) override;
    void on_read(media_read_batch batch) override;
    void on_end() override;

    void set_error_handle(std::function<void(boost::system::error_code)> handle) { error_handle_ = std::move(handle); }

   private:
    friend class rtsp_output_session;

    struct track_state
    {
        rtsp_output_track_description description;
        int payload_index{-1};
        int media_id{-1};
        int rtp_channel{-1};
        int rtcp_channel{-1};
    };

    int startup(rtsp_server_t* server,
                 std::string_view uri,
                 std::string_view session,
                 const rtsp_header_transport_t transports[],
                 std::size_t count,
                 std::unique_ptr<video_transcoder>& video_transcoder);
    static int muxer_packet_callback(void* param, int pid, const void* data, int bytes, std::uint32_t timestamp, int flags);
    void on_interleaved(std::uint8_t channel, std::span<const std::uint8_t> data);
    void safe_shutdown();
    bool create_muxer();
    bool apply_tracks(const media_track_snapshot_ptr& tracks);
    int on_setup(
        rtsp_server_t* server, std::string_view uri, std::string_view session, const rtsp_header_transport_t transports[], std::size_t count);
    int on_play(rtsp_server_t* server, std::string_view uri, std::string_view session, const std::int64_t* npt);
    int on_teardown(rtsp_server_t* server, std::string_view session);
    int on_muxer_packet(int pid, const void* data, int bytes);
    [[nodiscard]] bool description_current() const;
    [[nodiscard]] bool channels_available(track_id id, int rtp_channel, int rtcp_channel) const;

    boost::asio::any_io_executor executor_;
    std::function<void(std::span<const std::uint8_t>)> write_;
    std::function<void(boost::system::error_code)> error_handle_;
    std::shared_ptr<media_stream> stream_;
    rtsp_muxer_t* muxer_{};
    media_reader_handle reader_;
    std::map<track_id, track_state> tracks_;
    std::unique_ptr<video_transcoder> video_transcoder_;
    track_id video_track_id_{};
    media_reader_cursor reader_cursor_;
    std::uint64_t track_revision_{};
    std::string session_id_;
    bool playing_{};
    bool closed_{};
};

}    // namespace media_server

#endif
