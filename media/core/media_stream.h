#ifndef MEDIA_CORE_MEDIA_STREAM_H
#define MEDIA_CORE_MEDIA_STREAM_H

#include <memory>
#include <string>
#include <vector>

#include "media/core/media_sink.h"
#include "media/core/worker_sink_dispatcher.h"

namespace media_server
{
class mpeg_ps_output;

class media_stream final : public std::enable_shared_from_this<media_stream>
{
   public:
    media_stream(std::string stream_id, worker_context& worker);
    ~media_stream();

    [[nodiscard]] const std::string& stream_id() const noexcept;
    [[nodiscard]] worker_context& worker() const noexcept;
    [[nodiscard]] const std::vector<media_track>& tracks() const noexcept;

    bool set_tracks(std::vector<media_track> tracks);
    void add_sink(std::shared_ptr<media_sink> sink);
    void remove_sink(media_sink* sink);
    void publish(media_frame frame);
    void end();

    [[nodiscard]] std::shared_ptr<mpeg_ps_output> ps_output();

   private:
    void add_sink_owner(std::shared_ptr<media_sink> sink, worker_context& worker);

    std::string stream_id_;
    worker_context& worker_;
    std::vector<media_track> tracks_;
    worker_sink_dispatcher<media_frame, media_sink, &media_sink::on_frame> dispatcher_;
    std::weak_ptr<mpeg_ps_output> ps_output_;
    bool ended_{};
};

}    // namespace media_server

#endif
