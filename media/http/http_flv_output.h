#ifndef MEDIA_HTTP_HTTP_FLV_OUTPUT_H
#define MEDIA_HTTP_HTTP_FLV_OUTPUT_H

#include "media/core/media_sink.h"
#include "media/flv/flv_output_muxer.h"

#include <functional>
#include <span>
#include <vector>

extern "C"
{
#include "flv-writer.h"
}

namespace media_server
{
class http_flv_output final : public media_sink
{
   public:
    using write_handler = std::function<void(std::span<const std::uint8_t>)>;
    using end_handler = std::function<void()>;

    http_flv_output(std::span<const media_track> tracks, write_handler on_write, end_handler on_end);
    ~http_flv_output() override;

    void on_track(const media_track& track) override;
    void on_frame(const media_frame& frame) override;
    void on_end() override;

   private:
    static int writer_callback(void* param, const flv_vec_t* vectors, int count);

    write_handler on_write_;
    end_handler on_end_;
    std::vector<track_id> track_ids_;
    void* writer_ = nullptr;
    flv_output_muxer muxer_;
    bool ended_{};
};
}    // namespace media_server

#endif
