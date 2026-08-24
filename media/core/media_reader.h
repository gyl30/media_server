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
struct media_reader_state;

using media_reader_cursor = std::optional<std::uint64_t>;

struct media_track_snapshot
{
    std::uint64_t revision{};
    std::vector<media_track> tracks;
};

using media_track_snapshot_ptr = std::shared_ptr<const media_track_snapshot>;

struct media_read_entry
{
    std::uint64_t config_version{};
    media_frame frame;
};

struct media_read_batch
{
    std::uint64_t next_cursor{};
    media_track_snapshot_ptr tracks;
    std::vector<media_read_entry> entries;
};

class media_reader_handle final
{
   public:
    media_reader_handle() = default;

    // cursor 由 reader worker 保存；每次最多只有一个 outstanding read。
    void async_read(media_reader_cursor cursor) const;
    // remove 立即使 active 失效，之后不再产生 tracks、read 或 end 回调。
    void remove() const;

   private:
    friend class media_stream;

    media_reader_handle(std::weak_ptr<media_stream> stream, std::shared_ptr<media_reader_state> state);

    std::weak_ptr<media_stream> stream_;
    std::shared_ptr<media_reader_state> state_;
};

class media_reader
{
   public:
    virtual ~media_reader() = default;

    // tracks 是当前 stream 轨道快照；reader 只处理自己订阅的轨道。
    // on_read 返回 cursor 之后最多 128 个连续 history entry；未订阅轨道由 reader worker 自行忽略。
    // end 撤销 pending read 和尚未执行的 read/tracks 回调，随后只调用一次 on_end。
    // remove 不产生终止回调，并优先于尚未执行的任何 posted 回调。
    virtual void on_tracks(media_track_snapshot_ptr tracks) = 0;
    virtual void on_read(media_read_batch batch) = 0;
    virtual void on_end() = 0;

   protected:
    [[nodiscard]] const media_reader_handle& reader_handle() const noexcept { return handle_; }

   private:
    friend class media_stream;

    media_reader_handle handle_;
};

}    // namespace media_server

#endif
