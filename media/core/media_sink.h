#ifndef MEDIA_CORE_MEDIA_SINK_H
#define MEDIA_CORE_MEDIA_SINK_H

#include <functional>
#include <memory>
#include <optional>
#include <vector>
#include <cstdint>

#include "media/core/media_types.h"

namespace media_server
{

class media_stream;
template <typename Frame>
class media_history;
template <typename Frame>
struct media_sink_state_t;
template <typename Frame>
struct media_worker_history_t;

using media_tracks = std::vector<media_track>;
using media_tracks_ptr = std::shared_ptr<const media_tracks>;

template <typename Frame>
class media_sink_t
{
   public:
    ~media_sink_t();

    void close() const;
    [[nodiscard]] std::optional<Frame> read();
    void async_wait(std::function<void(bool)> handler);


   private:
    friend class media_history<Frame>;
    friend struct media_worker_history_t<Frame>;

    std::weak_ptr<media_history<Frame>> history_;
    std::shared_ptr<media_sink_state_t<Frame>> state_;
    std::shared_ptr<media_worker_history_t<Frame>> worker_history_;
    std::optional<std::uint64_t> cursor_;
};

using media_sink = media_sink_t<media_frame>;

}    // namespace media_server

#endif
