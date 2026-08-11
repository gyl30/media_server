#ifndef MEDIA_CORE_MEDIA_READER_H
#define MEDIA_CORE_MEDIA_READER_H

#include "media/core/media_types.h"

#include <cstdint>
#include <memory>

namespace media_server
{

class media_stream;
struct media_reader_state;

using media_reader_generation = std::uint64_t;

class media_reader_handle final
{
   public:
    media_reader_handle() = default;

    // generation 必须来自当前 on_ready/on_read 回调；旧网络 completion 使用旧 generation 请求时会被忽略。
    void async_read(media_reader_generation generation) const;
    // remove 立即使 active 失效，之后不再产生 track、read 或 end 回调。
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

    // 注册和每次 track reset 都开始一个新 generation，严格按完整 on_track*、on_ready、on_read* 执行。
    // on_ready 前的 read 无效；每次 read 只返回一个 frame，下一次 read 必须携带该 frame 的 generation。
    // end 切换到终止 generation，撤销 pending read 和旧 posted 回调，随后只调用一次 on_end。
    // remove 不产生终止回调，并优先于尚未执行的任何 posted 回调。
    virtual void on_track(media_reader_generation generation, const media_track& track) = 0;
    virtual void on_ready(media_reader_generation generation) = 0;
    virtual void on_read(media_reader_generation generation, media_frame frame) = 0;
    virtual void on_end(media_reader_generation generation) = 0;

   protected:
    [[nodiscard]] const media_reader_handle& reader_handle() const noexcept { return handle_; }

   private:
    friend class media_stream;

    media_reader_handle handle_;
};

}    // namespace media_server

#endif
