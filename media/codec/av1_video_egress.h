#ifndef MEDIA_CODEC_AV1_VIDEO_EGRESS_H
#define MEDIA_CODEC_AV1_VIDEO_EGRESS_H

#include <atomic>
#include <map>
#include <memory>
#include <optional>

#include "media/codec/video_transcoder.h"
#include "media/core/media_reader.h"
#include "media/core/media_stream.h"

namespace media_server
{
class av1_video_egress final : public media_reader, public std::enable_shared_from_this<av1_video_egress>
{
   public:
    [[nodiscard]] std::shared_ptr<media_stream> stream() const noexcept;

    void on_tracks(media_track_snapshot_ptr tracks) override;
    void on_read_ready(media_track_snapshot_ptr tracks, bool waited_for_media) override;
    void on_end() override;

   private:
    friend std::shared_ptr<av1_video_egress> acquire_av1_video_egress(
        const std::shared_ptr<media_stream>& source, worker_context& worker, std::optional<av1_encoding_parameters> parameters);
    friend void release_av1_video_egress(std::shared_ptr<av1_video_egress>& egress);

    av1_video_egress(std::shared_ptr<media_stream> source, worker_context& worker, std::optional<av1_encoding_parameters> parameters);
    bool startup(const std::vector<media_track>& tracks);
    bool startup_transcoder(const media_track& track);
    void finish();

    worker_context& worker_;
    std::shared_ptr<media_stream> source_;
    std::shared_ptr<media_stream> output_;
    std::optional<av1_encoding_parameters> parameters_;
    std::map<track_id, media_track> source_tracks_;
    std::unique_ptr<video_transcoder> transcoder_;
    track_id video_track_id_{};
    std::atomic_bool ended_{};
    std::uint64_t source_revision_{};
    bool reading_{};
    bool waiting_for_source_key_frame_{true};
    std::size_t viewers_{};
    worker_context::shutdown_subscription shutdown_subscription_;
};

[[nodiscard]] std::shared_ptr<av1_video_egress> acquire_av1_video_egress(
    const std::shared_ptr<media_stream>& source, worker_context& worker, std::optional<av1_encoding_parameters> parameters);
void release_av1_video_egress(std::shared_ptr<av1_video_egress>& egress);

}    // namespace media_server

#endif
