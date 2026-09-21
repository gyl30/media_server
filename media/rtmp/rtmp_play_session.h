#ifndef MEDIA_RTMP_RTMP_PLAY_SESSION_H
#define MEDIA_RTMP_RTMP_PLAY_SESSION_H

#include <map>
#include <memory>
#include <string>
#include <cstdint>
#include <cstddef>
#include <functional>
#include <string_view>

#include "media/flv/flv_muxer.h"
#include "media/core/media_reader.h"
#include "media/core/media_stream.h"

namespace media_server
{

class worker_context;

class rtmp_play_session final : public media_reader, public std::enable_shared_from_this<rtmp_play_session>
{
   public:
    using end_handler = std::function<void()>;
    using queue_bytes_handler = std::function<std::size_t()>;

    rtmp_play_session(worker_context& worker,
                      std::string stream_id,
                      std::string stream_name,
                      std::shared_ptr<media_stream> stream,
                      flv_muxer::packet_handler packet_handler,
                      video_transcode_config video,
                      end_handler on_end,
                      queue_bytes_handler queued_output_bytes,
                      std::size_t max_output_queue_bytes);

   public:
    void startup();
    void shutdown();

   public:
    [[nodiscard]] std::string_view stream_id() const noexcept { return stream_id_; }
    [[nodiscard]] std::string_view stream_name() const noexcept { return stream_name_; }
    [[nodiscard]] bool waiting_for_output() const noexcept { return waiting_for_output_; }
    void on_output_progress();

   public:
    void on_tracks(media_track_snapshot_ptr tracks) override;
    void on_read(media_read_batch batch) override;
    void on_end() override;

   private:
    void process_batch();
    [[nodiscard]] std::size_t queued_output_bytes() const;
    [[nodiscard]] bool output_backpressured() const;
    [[nodiscard]] bool output_drained() const;

    void apply_tracks(const media_track_snapshot_ptr& tracks);

   private:
    worker_context& worker_;
    std::string stream_id_;
    std::string stream_name_;
    std::shared_ptr<media_stream> stream_;
    flv_muxer muxer_;
    queue_bytes_handler queued_output_bytes_;
    std::size_t max_output_queue_bytes_{};
    end_handler end_handler_;
    std::map<track_id, media_track> reader_tracks_;
    media_reader_cursor reader_cursor_;
    std::uint64_t track_revision_{};
    media_read_batch batch_;
    std::size_t batch_index_{};
    bool waiting_for_key_frame_{};
    bool closed_{};
    bool waiting_for_output_{};
};

}    // namespace media_server

#endif
