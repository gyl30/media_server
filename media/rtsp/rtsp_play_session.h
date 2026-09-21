#ifndef MEDIA_RTSP_RTSP_PLAY_SESSION_H
#define MEDIA_RTSP_RTSP_PLAY_SESSION_H

#include <map>
#include <span>
#include <memory>
#include <string>
#include <cstdint>
#include <cstddef>
#include <utility>
#include <vector>
#include <functional>
#include <string_view>

#include <boost/asio/ip/address.hpp>

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
    using write_handler = std::function<void(std::vector<std::uint8_t>)>;
    using queue_bytes_handler = std::function<std::size_t()>;
    rtsp_play_session(worker_context& worker,
                      std::string stream_id,
                      std::string stream_name,
                      video_transcode_codec video_codec,
                      boost::asio::ip::address local_address,
                      write_handler write,
                      queue_bytes_handler queued_output_bytes,
                      std::size_t max_output_queue_bytes);

   public:
    void set_shutdown_handler(std::function<void()> handler) { shutdown_handler_ = std::move(handler); }

   public:
    void startup();
    void shutdown();

   public:
    [[nodiscard]] std::string_view stream_id() const noexcept { return stream_id_; }
    [[nodiscard]] std::string_view stream_name() const noexcept { return stream_name_; }
    [[nodiscard]] bool waiting_for_output() const noexcept { return waiting_for_output_; }
    void on_output_progress();

   public:
    [[nodiscard]] bool on_interleaved(std::uint8_t channel, std::span<const std::uint8_t> data);
    int on_describe(rtsp_server_t* server, std::string_view uri);
    int on_setup(
        rtsp_server_t* server, std::string_view uri, std::string_view session, const rtsp_header_transport_t transports[], std::size_t count);
    int on_play(rtsp_server_t* server, std::string_view uri, std::string_view session, const std::int64_t* npt, const double* scale);
    int on_teardown(rtsp_server_t* server, std::string_view uri, std::string_view session);

   public:
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

   private:
    static int muxer_packet_callback(void* param, int pid, const void* data, int bytes, std::uint32_t timestamp, int flags);

   private:
    [[nodiscard]] int prepare_presentation();
    [[nodiscard]] bool apply_tracks(const media_track_snapshot_ptr& tracks);
    int on_muxer_packet(int pid, const void* data, int bytes);
    void write_interleaved(std::uint8_t channel, const void* data, std::size_t bytes);
    [[nodiscard]] int presentation_status() const;
    void process_batch();
    [[nodiscard]] std::size_t queued_output_bytes() const;
    [[nodiscard]] bool output_backpressured() const;
    [[nodiscard]] bool output_drained() const;
    [[nodiscard]] bool channels_available(track_id id, int rtp_channel, int rtcp_channel) const;

   private:
    void safe_shutdown();

   private:
    worker_context& worker_;
    std::string stream_id_;
    std::string stream_name_;
    video_transcode_codec video_codec_;
    boost::asio::ip::address local_address_;
    write_handler write_handler_;
    std::function<void()> shutdown_handler_;
    std::shared_ptr<media_stream> stream_;
    queue_bytes_handler queued_output_bytes_;
    std::size_t max_output_queue_bytes_{};
    std::map<track_id, track_state> track_states_;
    std::unique_ptr<video_transcoder> video_transcoder_;
    rtsp_muxer_t* muxer_{};
    track_id video_track_id_{};
    media_reader_cursor reader_cursor_;
    std::uint64_t track_revision_{};
    std::string session_id_;
    bool playing_{};
    media_read_batch batch_;
    std::size_t batch_index_{};
    bool closed_{};
    bool waiting_for_output_{};
};

}    // namespace media_server

#endif
