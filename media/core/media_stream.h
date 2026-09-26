#ifndef MEDIA_CORE_MEDIA_STREAM_H
#define MEDIA_CORE_MEDIA_STREAM_H

#include "media/core/media_history.h"
#include "media/core/media_sink.h"

namespace media_server
{
class mpeg_ps_output;

class media_stream final : public media_history<media_frame>
{
   public:
    media_stream(std::string name, worker_context& worker);

    void add_sink(const std::shared_ptr<media_sink>& sink);
    bool set_tracks(std::vector<media_track> tracks);
    bool update_track(media_track track);
    void publish(media_frame frame);
    void end();

    [[nodiscard]] worker_context& worker() const noexcept;
    // 只由 source worker 调用；缓存只属于当前 source generation。
    [[nodiscard]] std::shared_ptr<mpeg_ps_output> ps_output();

   private:
    void attach_sink(std::shared_ptr<media_sink> sink);
    void replay_to(media_sink& sink);

    std::vector<std::shared_ptr<media_sink>> sinks_;
    std::weak_ptr<mpeg_ps_output> ps_output_;
    std::uint64_t sink_replay_barrier_sequence_{};
};

}    // namespace media_server

#endif
