#ifndef MEDIA_HTTP_HTTP_FLV_OUTPUT_H
#define MEDIA_HTTP_HTTP_FLV_OUTPUT_H

#include <map>
#include <vector>
#include <functional>

#include "media/core/media_reader.h"
#include "media/flv/flv_output_muxer.h"

extern "C"
{
#include "flv-writer.h"
}

namespace media_server
{
class http_flv_output final : public media_reader
{
   public:
    using write_handler = std::function<void(std::uint64_t, std::vector<std::uint8_t>, bool)>;
    using end_handler = std::function<void()>;

    http_flv_output(write_handler on_write, end_handler on_end, output_video_config video = {});
    ~http_flv_output() override;

    void on_tracks(media_track_snapshot_ptr tracks) override;
    void on_read(media_read_batch batch) override;
    void on_end() override;
    void shutdown();
    void write_complete(std::uint64_t generation);

   private:
    static int writer_callback(void* param, const flv_vec_t* vectors, int count);
    bool apply_tracks(const media_track_snapshot_ptr& tracks);
    void process_batch();
    void finish();

    write_handler on_write_;
    end_handler on_end_;
    std::map<track_id, media_track> reader_tracks_;
    media_read_batch batch_;
    std::vector<std::uint8_t> output_buffer_;
    void* writer_ = nullptr;
    flv_output_muxer muxer_;
    std::uint64_t generation_{};
    media_reader_cursor reader_cursor_;
    std::uint64_t track_revision_{};
    std::size_t batch_index_{};
    bool batch_active_{};
    bool waiting_for_key_frame_{};
    bool ended_{};
};
}    // namespace media_server

#endif
