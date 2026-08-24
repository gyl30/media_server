#ifndef MEDIA_RTMP_RTMP_OUTPUT_SESSION_H
#define MEDIA_RTMP_RTMP_OUTPUT_SESSION_H

#include <map>
#include <memory>
#include <cstdint>
#include <functional>

#include <boost/asio/any_io_executor.hpp>

#include "media/core/media_reader.h"
#include "media/core/media_stream.h"
#include "media/flv/flv_output_muxer.h"

namespace media_server
{

class rtmp_output_session final : public media_reader, public std::enable_shared_from_this<rtmp_output_session>
{
   public:
    using end_handler = std::function<void()>;

    rtmp_output_session(boost::asio::any_io_executor executor,
                        std::shared_ptr<media_stream> stream,
                        flv_output_muxer::output_handler output,
                        output_video_config video,
                        end_handler on_end);

    void startup();
    void shutdown();

    void on_tracks(media_track_snapshot_ptr tracks) override;
    void on_read(media_read_batch batch) override;
    void on_end() override;

   private:
    void apply_tracks(const media_track_snapshot_ptr& tracks);

    boost::asio::any_io_executor executor_;
    std::shared_ptr<media_stream> stream_;
    flv_output_muxer output_muxer_;
    end_handler on_end_;
    media_reader_handle reader_;
    std::map<track_id, media_track> reader_tracks_;
    media_reader_cursor reader_cursor_;
    std::uint64_t track_revision_{};
    bool waiting_for_key_frame_{};
    bool closed_{};
};

}    // namespace media_server

#endif
