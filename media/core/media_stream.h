#ifndef MEDIA_CORE_MEDIA_STREAM_H
#define MEDIA_CORE_MEDIA_STREAM_H

#include "media/core/media_sink.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace media_server
{

class media_stream final
{
   public:
    explicit media_stream(std::string name);

    [[nodiscard]] const std::string& name() const noexcept;
    [[nodiscard]] bool ended() const noexcept;
    [[nodiscard]] std::vector<media_track> tracks() const;

    bool add_sink(const std::shared_ptr<media_sink>& sink);
    void remove_sink(const media_sink& sink);
    // 仅在新增轨道或实际配置变化时返回 true。
    bool update_track(media_track track);
    bool publish(media_frame frame);
    void end();

   private:
    [[nodiscard]] bool has_sink(const media_sink& sink) const;
    [[nodiscard]] std::vector<std::shared_ptr<media_sink>> sink_snapshot();
    void remove_expired_sinks();

    std::string name_;
    std::map<track_id, media_track> tracks_;
    std::vector<std::weak_ptr<media_sink>> sinks_;
    std::vector<media_frame> gop_cache_;
    bool ended_{};
};

}    // namespace media_server

#endif
