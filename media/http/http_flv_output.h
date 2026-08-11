#ifndef MEDIA_HTTP_HTTP_FLV_OUTPUT_H
#define MEDIA_HTTP_HTTP_FLV_OUTPUT_H

#include "media/core/media_reader.h"
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
class http_flv_output final : public media_reader
{
   public:
    using write_handler = std::function<void(media_reader_generation, std::vector<std::uint8_t>, bool)>;
    using end_handler = std::function<void()>;

    http_flv_output(write_handler on_write, end_handler on_end);
    ~http_flv_output() override;

    void on_track(media_reader_generation generation, const media_track& track) override;
    void on_ready(media_reader_generation generation) override;
    void on_read(media_reader_generation generation, media_frame frame) override;
    void on_end(media_reader_generation generation) override;
    void write_complete(media_reader_generation generation);

   private:
    static int writer_callback(void* param, const flv_vec_t* vectors, int count);
    void finish();

    write_handler on_write_;
    end_handler on_end_;
    std::vector<track_id> track_ids_;
    std::vector<media_track> pending_tracks_;
    std::vector<std::uint8_t> output_buffer_;
    void* writer_ = nullptr;
    flv_output_muxer muxer_;
    media_reader_generation generation_{};
    bool ended_{};
};
}    // namespace media_server

#endif
