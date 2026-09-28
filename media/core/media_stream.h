#ifndef MEDIA_CORE_MEDIA_STREAM_H
#define MEDIA_CORE_MEDIA_STREAM_H

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "media/core/media_sink.h"

namespace media_server
{
class mpeg_ps_output;

class media_stream final : public std::enable_shared_from_this<media_stream>
{
   public:
    media_stream(std::string name, worker_context& worker);
    ~media_stream();

    [[nodiscard]] const std::string& name() const noexcept;
    [[nodiscard]] worker_context& worker() const noexcept;
    [[nodiscard]] const std::vector<media_track>& tracks() const noexcept;

    bool set_tracks(std::vector<media_track> tracks);
    void add_sink(std::shared_ptr<media_sink> sink, worker_context& worker);
    void remove_sink(media_sink* sink);
    void publish(media_frame frame);
    void end();

    [[nodiscard]] std::shared_ptr<mpeg_ps_output> ps_output();

   private:
    struct sink_group;

    void add_sink_owner(std::shared_ptr<media_sink> sink, worker_context& worker);
    void remove_sink_owner(media_sink* sink);
    void end_group(const std::shared_ptr<sink_group>& group);

    std::string name_;
    worker_context& worker_;
    std::vector<media_track> tracks_;
    std::map<worker_context*, std::shared_ptr<sink_group>> sink_groups_;
    std::weak_ptr<mpeg_ps_output> ps_output_;
    bool ended_{};
};

}    // namespace media_server

#endif
