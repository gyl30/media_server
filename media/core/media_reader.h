#ifndef MEDIA_CORE_MEDIA_READER_H
#define MEDIA_CORE_MEDIA_READER_H

#include <memory>
#include <vector>
#include <cstdint>
#include <cstddef>
#include <optional>
#include <utility>

#include "media/core/media_types.h"

namespace media_server
{

class media_stream;
template <typename Frame>
class media_history;
template <typename Frame>
struct media_reader_state_t;

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
    virtual ~media_reader_t() = default;

   public:
    // tracks 是当前 stream 轨道快照；reader 只处理自己订阅的轨道。
    // end 撤销 pending read 和尚未执行的 read/tracks 回调，随后只调用一次 on_end。
    // remove 不产生终止回调，并优先于尚未执行的任何 posted 回调。
    virtual void on_tracks(media_track_snapshot_ptr tracks) = 0;
    virtual void on_end() = 0;
    // 立即使 active 失效，之后不再产生 tracks、read 或 end 回调。
    void remove_reader() const;

   protected:
    // 当前 batch 耗尽时请求下一批 history，并在新媒体到达前返回空。
    [[nodiscard]] std::optional<media_read_entry_t<Frame>> read();
    virtual void on_read_ready(media_track_snapshot_ptr, bool) {}

   private:
    friend class media_history<Frame>;

    struct read_batch
    {
        std::uint64_t next_cursor{};
        media_track_snapshot_ptr tracks;
        std::vector<media_read_entry_t<Frame>> entries;
        bool waited_for_media{};
    };

    void request_read() const;
    void deliver_read_batch(read_batch batch)
    {
        reader_cursor_ = batch.next_cursor;
        batch_ = std::move(batch);
        batch_index_ = 0;
        on_read_ready(batch_.tracks, batch_.waited_for_media);
    }

   private:
    std::weak_ptr<media_history<Frame>> history_;
    std::shared_ptr<media_reader_state_t<Frame>> state_;
    std::optional<std::uint64_t> reader_cursor_;
    read_batch batch_;
    std::size_t batch_index_{};
};

using media_read_entry = media_read_entry_t<media_frame>;
using media_reader = media_reader_t<media_frame>;

}    // namespace media_server

#endif
