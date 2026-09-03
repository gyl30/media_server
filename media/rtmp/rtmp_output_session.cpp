#include <utility>

#include "media/rtmp/rtmp_output_session.h"
#include "media/net/worker_context.h"

namespace media_server
{

rtmp_output_session::rtmp_output_session(worker_context& worker,
                                         std::shared_ptr<media_stream> stream,
                                         flv_output_muxer::output_handler output,
                                         output_video_config video,
                                         end_handler on_end)
    : worker_(worker), stream_(std::move(stream)), output_muxer_(std::move(output), video), end_handler_(std::move(on_end))
{
}

void rtmp_output_session::startup()
{
    if (closed_ || !stream_)
    {
        return;
    }
    reader_ = stream_->add_reader(shared_from_this(), worker_.io());
}

void rtmp_output_session::shutdown()
{
    if (closed_)
    {
        return;
    }
    closed_ = true;
    reader_.remove();
    reader_ = {};
    reader_cursor_.reset();
    reader_tracks_.clear();
    track_revision_ = 0;
    waiting_for_key_frame_ = false;
    output_muxer_.shutdown();
    stream_.reset();
}

void rtmp_output_session::on_tracks(media_track_snapshot_ptr tracks)
{
    if (closed_)
    {
        return;
    }

    apply_tracks(tracks);
    if (!closed_)
    {
        reader_handle().async_read(reader_cursor_);
    }
}

void rtmp_output_session::on_read(media_read_batch batch)
{
    if (closed_)
    {
        return;
    }

    reader_cursor_ = batch.next_cursor;
    apply_tracks(batch.tracks);
    if (closed_)
    {
        return;
    }

    for (auto& entry : batch.entries)
    {
        const auto track = reader_tracks_.find(entry.frame.track);
        if (track == reader_tracks_.end() || track->second.config_version != entry.config_version)
        {
            continue;
        }

        if (waiting_for_key_frame_)
        {
            if (track->second.kind != media_kind::video || !entry.frame.key_frame)
            {
                continue;
            }
            waiting_for_key_frame_ = false;
        }
        output_muxer_.on_frame(entry.frame);
    }

    reader_handle().async_read(reader_cursor_);
}

void rtmp_output_session::on_end()
{
    if (!closed_)
    {
        end_handler_();
    }
}

void rtmp_output_session::apply_tracks(const media_track_snapshot_ptr& tracks)
{
    if (!tracks || tracks->revision <= track_revision_)
    {
        return;
    }

    bool video_changed = false;
    if (track_revision_ != 0)
    {
        for (const auto& track : tracks->tracks)
        {
            const auto current = reader_tracks_.find(track.id);
            if (current != reader_tracks_.end() && track.kind == media_kind::video && current->second.config_version != track.config_version)
            {
                video_changed = true;
            }
        }
    }

    reader_tracks_.clear();
    for (const auto& track : tracks->tracks)
    {
        reader_tracks_.emplace(track.id, track);
        output_muxer_.on_track(track);
    }
    track_revision_ = tracks->revision;
    waiting_for_key_frame_ = waiting_for_key_frame_ || video_changed;
}

}    // namespace media_server
