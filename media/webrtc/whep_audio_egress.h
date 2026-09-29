#ifndef MEDIA_WEBRTC_WHEP_AUDIO_EGRESS_H
#define MEDIA_WEBRTC_WHEP_AUDIO_EGRESS_H

#include <atomic>
#include <map>
#include <memory>
#include <vector>

#include "media/core/media_sink.h"
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

class whep_audio_egress final : public media_sink, public std::enable_shared_from_this<whep_audio_egress>
{
   public:
    [[nodiscard]] std::shared_ptr<media_stream> stream() const noexcept;

    [[nodiscard]] worker_context& worker() noexcept override { return worker_; }
    void on_frame(const media_frame& frame) override;
    void on_end() override;

   private:
    friend std::shared_ptr<whep_audio_egress> acquire_whep_audio_egress(
        const std::shared_ptr<media_stream>& source, worker_context& worker, whep_audio_settings settings);
    friend void release_whep_audio_egress(std::shared_ptr<whep_audio_egress>& egress);

    whep_audio_egress(std::shared_ptr<media_stream> source, worker_context& worker);
    bool startup(const std::vector<media_track>& tracks, whep_audio_settings settings);
    void finish();

    worker_context& worker_;
    std::shared_ptr<media_stream> source_;
    std::shared_ptr<media_stream> output_;
    std::map<track_id, std::unique_ptr<audio_transcoder>> transcoders_;
    std::atomic_bool finished_{};
    std::size_t viewers_{};
    worker_context::shutdown_subscription shutdown_subscription_;
};

[[nodiscard]] std::shared_ptr<whep_audio_egress> acquire_whep_audio_egress(
    const std::shared_ptr<media_stream>& source, worker_context& worker, whep_audio_settings settings);
void release_whep_audio_egress(std::shared_ptr<whep_audio_egress>& egress);

}    // namespace media_server

#endif
