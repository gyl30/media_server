#ifndef MEDIA_CORE_MEDIA_READER_H
#define MEDIA_CORE_MEDIA_READER_H

#include <memory>
#include <vector>
#include <cstdint>
#include <optional>

#include "media/core/media_types.h"

namespace media_server
{

class media_stream;
template <typename Frame>
class media_history;
template <typename Frame>
struct media_reader_state_t;
template <typename Frame>
struct media_worker_history_t;

struct media_track_snapshot
{
    std::uint64_t revision{};
    std::vector<media_track> tracks;
};

using media_track_snapshot_ptr = std::shared_ptr<const media_track_snapshot>;

template <typename Frame>
struct media_read_entry_t
{
    std::uint64_t config_version{};
    Frame frame;
};

template <typename Frame>
class media_reader_t
{
   public:
    virtual ~media_reader_t();

   public:
    // tracks 是当前 stream 轨道快照；reader 只处理自己订阅的轨道。
    // remove 不产生终止回调，并优先于尚未执行的任何 posted 回调。
    virtual void on_tracks(media_track_snapshot_ptr tracks) = 0;
    virtual void on_end() = 0;
    // 立即使 active 失效，之后不再产生 tracks、read 或 end 回调。
    void remove_reader() const;

   protected:
    // 在 reader worker 上从本地有界 history 读取；无新媒体时返回空。
    [[nodiscard]] std::optional<media_read_entry_t<Frame>> read();
    virtual void on_media_available(bool waited_for_media) = 0;

   private:
    friend class media_history<Frame>;
    friend struct media_worker_history_t<Frame>;

   private:
    std::weak_ptr<media_history<Frame>> history_;
    std::shared_ptr<media_reader_state_t<Frame>> state_;
    std::shared_ptr<media_worker_history_t<Frame>> worker_history_;
    std::optional<std::uint64_t> reader_cursor_;
};

using media_read_entry = media_read_entry_t<media_frame>;
using media_reader = media_reader_t<media_frame>;

}    // namespace media_server

#endif
