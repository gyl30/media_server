#ifndef MEDIA_WEBRTC_WHEP_AUDIO_EGRESS_H
#define MEDIA_WEBRTC_WHEP_AUDIO_EGRESS_H

#include <atomic>
#include <map>
#include <memory>
#include <vector>

#include "media/core/media_reader.h"
#include "media/core/media_stream.h"
#include "media/codec/audio_transcoder.h"

namespace media_server
{
class worker_context;

struct whep_audio_settings
{
    int channels{};
    int bitrate{};
    int max_playback_rate{};
};

class whep_audio_egress final : public media_reader, public std::enable_shared_from_this<whep_audio_egress>
{
   public:
    enum class end_reason
    {
        none,
        source_ended,
        source_changed,
        transcode_failed,
        unused,
        worker_stopped,
    };

    [[nodiscard]] std::shared_ptr<media_stream> stream() const noexcept;
    [[nodiscard]] end_reason reason() const noexcept;

    void on_tracks(media_track_snapshot_ptr tracks) override;
    void on_read(media_read_batch batch) override;
    void on_end() override;

   private:
    friend std::shared_ptr<whep_audio_egress> acquire_whep_audio_egress(
        const std::shared_ptr<media_stream>& source, worker_context& worker, whep_audio_settings settings);
    friend void release_whep_audio_egress(std::shared_ptr<whep_audio_egress>& egress);

    whep_audio_egress(std::shared_ptr<media_stream> source, worker_context& worker);
    bool startup(const std::vector<media_track>& tracks, whep_audio_settings settings);
    [[nodiscard]] bool matches(const std::vector<media_track>& tracks) const;
    void finish(end_reason reason);

    worker_context& worker_;
    std::shared_ptr<media_stream> source_;
    std::shared_ptr<media_stream> output_;
    std::map<track_id, media_track> source_tracks_;
    std::map<track_id, std::unique_ptr<audio_transcoder>> transcoders_;
    media_reader_cursor cursor_;
    std::atomic<end_reason> reason_{end_reason::none};
    std::uint64_t source_revision_{};
    bool reading_{};
    std::size_t viewers_{};
    worker_context::shutdown_subscription shutdown_subscription_;
};

[[nodiscard]] std::shared_ptr<whep_audio_egress> acquire_whep_audio_egress(
    const std::shared_ptr<media_stream>& source, worker_context& worker, whep_audio_settings settings);
void release_whep_audio_egress(std::shared_ptr<whep_audio_egress>& egress);

}    // namespace media_server

#endif
