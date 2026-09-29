#include <utility>

#include "media/net/worker_context.h"
#include "media/rtmp/rtmp_play_session.h"

namespace media_server
{

rtmp_play_session::rtmp_play_session(worker_context& worker,
                                     std::shared_ptr<media_stream> stream,
                                     flv_muxer::packet_handler packet_handler,
                                     end_handler handle_source_end)
    : worker_(worker),
      stream_(std::move(stream)),
      muxer_(std::move(packet_handler)),
      end_handler_(std::move(handle_source_end))
{
}

void rtmp_play_session::startup()
{
    if (closed_ || !stream_)
    {
        return;
    }
    for (const auto& track : stream_->tracks())
    {
        muxer_.on_track(track);
        if (track.kind == media_kind::video)
        {
            video_track_ = track.id;
        }
    }
    waiting_for_key_frame_ = video_track_.has_value();
    stream_->add_sink(shared_from_this());
}

void rtmp_play_session::shutdown()
{
    if (closed_)
    {
        return;
    }
    closed_ = true;
    if (stream_)
    {
        stream_->remove_sink(this);
        stream_.reset();
    }
    video_track_.reset();
    waiting_for_key_frame_ = false;
    muxer_.shutdown();
}

void rtmp_play_session::on_frame(const media_frame& frame)
{
    if (closed_)
    {
        return;
    }

    if (waiting_for_key_frame_)
    {
        if (frame.track != *video_track_ || !frame.key_frame)
        {
            return;
        }
        waiting_for_key_frame_ = false;
    }

    muxer_.on_frame(frame);
}

void rtmp_play_session::on_end()
{
    if (closed_)
    {
        return;
    }
    end_handler_();
}

}    // namespace media_server
