#include <utility>

#include "media/rtmp/rtmp_event.h"
#include "media/net/worker_context.h"
#include "media/rtmp/rtmp_play_session.h"

namespace media_server
{

rtmp_play_session::rtmp_play_session(worker_context& worker,
                                     std::string stream_id,
                                     std::string stream_name,
                                     std::shared_ptr<media_stream> stream,
                                     flv_muxer::packet_handler packet_handler,
                                     video_transcode_config video,
                                     end_handler on_end,
                                     queue_bytes_handler queued_output_bytes,
                                     std::size_t max_output_queue_bytes)
    : worker_(worker),
      stream_id_(std::move(stream_id)),
      stream_name_(std::move(stream_name)),
      stream_(std::move(stream)),
      muxer_(std::move(packet_handler), video),
      queued_output_bytes_(std::move(queued_output_bytes)),
      max_output_queue_bytes_(max_output_queue_bytes),
      end_handler_(std::move(on_end))
{
}

void rtmp_play_session::startup()
{
    if (closed_ || !stream_)
    {
        return;
    }
    stream_->add_reader(shared_from_this(), worker_);
    rtmp_event::report_output(event_state::streaming, stream_id_, stream_name_, "streaming");
}

void rtmp_play_session::shutdown()
{
    if (closed_)
    {
        return;
    }
    closed_ = true;
    remove_reader();
    reader_tracks_.clear();
    track_revision_ = 0;
    waiting_for_key_frame_ = false;
    muxer_.shutdown();
    stream_.reset();
    rtmp_event::report_output(event_state::stopped, stream_id_, stream_name_);
    waiting_for_output_ = false;
}

void rtmp_play_session::on_tracks(media_track_snapshot_ptr tracks)
{
    if (closed_)
    {
        return;
    }

    apply_tracks(tracks);
    if (!closed_ && !waiting_for_output_)
    {
        process_read(false);
    }
}

void rtmp_play_session::on_read_ready(media_track_snapshot_ptr tracks, bool waited_for_media)
{
    if (closed_)
    {
        return;
    }

    apply_tracks(tracks);
    if (closed_)
    {
        return;
    }
    process_read(!waited_for_media);
}

void rtmp_play_session::on_end()
{
    if (!closed_)
    {
        waiting_for_output_ = false;
        rtmp_event::report_output(event_state::remote_closed, stream_id_, stream_name_, "media");
        end_handler_();
    }
}

void rtmp_play_session::on_output_progress()
{
    if (closed_ || !waiting_for_output_ || !output_drained())
    {
        return;
    }
    waiting_for_output_ = false;
    process_read(true);
}

void rtmp_play_session::process_read(bool replaying_history)
{
    while (!closed_)
    {
        if (replaying_history && output_backpressured())
        {
            waiting_for_output_ = true;
            return;
        }

        auto entry = read();
        if (!entry)
        {
            return;
        }
        const auto track = reader_tracks_.find(entry->frame.track);
        if (track == reader_tracks_.end() || track->second.config_version != entry->config_version)
        {
            continue;
        }

        if (waiting_for_key_frame_)
        {
            if (track->second.kind != media_kind::video || !entry->frame.key_frame)
            {
                continue;
            }
            waiting_for_key_frame_ = false;
        }
        muxer_.on_frame(entry->frame);
    }
}

std::size_t rtmp_play_session::queued_output_bytes() const
{
    return queued_output_bytes_();
}

// history replay 只使用半个 queue；恢复水位形成滞回，并为单帧展开及 control output 保留余量。
bool rtmp_play_session::output_backpressured() const { return queued_output_bytes() >= max_output_queue_bytes_ / 2U; }

bool rtmp_play_session::output_drained() const { return queued_output_bytes() <= max_output_queue_bytes_ / 4U; }

void rtmp_play_session::apply_tracks(const media_track_snapshot_ptr& tracks)
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
        muxer_.on_track(track);
    }
    track_revision_ = tracks->revision;
    waiting_for_key_frame_ = waiting_for_key_frame_ || video_changed;
}

}    // namespace media_server
