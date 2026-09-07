#ifndef MEDIA_RTMP_RTMP_PLAY_SESSION_H
#define MEDIA_RTMP_RTMP_PLAY_SESSION_H

#include <map>
#include <memory>
#include <cstdint>
#include <functional>

#include "media/core/media_reader.h"
#include "media/core/media_stream.h"
#include "media/flv/flv_muxer.h"

namespace media_server
{

class worker_context;

class rtmp_play_session final : public media_reader, public std::enable_shared_from_this<rtmp_play_session>
{
   public:
    using end_handler = std::function<void()>;

    rtmp_play_session(worker_context& worker,
                        std::shared_ptr<media_stream> stream,
                        flv_muxer::packet_handler packet_handler,
                        video_transcode_config video,
                        end_handler on_end);

    void startup();
    void shutdown();

    void on_tracks(media_track_snapshot_ptr tracks) override;
    void on_read(media_read_batch batch) override;
    void on_end() override;

   private:
    void apply_tracks(const media_track_snapshot_ptr& tracks);

    worker_context& worker_;
    std::shared_ptr<media_stream> stream_;
    flv_muxer muxer_;
    end_handler end_handler_;
    media_reader_handle reader_;
    std::map<track_id, media_track> reader_tracks_;
    media_reader_cursor reader_cursor_;
    std::uint64_t track_revision_{};
    bool waiting_for_key_frame_{};
    bool closed_{};
};

}    // namespace media_server

#endif
