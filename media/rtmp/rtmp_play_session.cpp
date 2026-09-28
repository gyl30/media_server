#include <utility>

#include "media/net/worker_context.h"
#include "media/rtmp/rtmp_play_session.h"

namespace media_server
{

rtmp_play_session::rtmp_play_session(worker_context& worker,
                                     std::shared_ptr<media_stream> stream,
                                     flv_muxer::packet_handler packet_handler,
                                     end_handler handle_source_end,
                                     queue_bytes_handler queued_output_bytes,
                                     std::size_t max_output_queue_bytes)
    : worker_(worker),
      stream_(std::move(stream)),
      muxer_(std::move(packet_handler)),
      queued_output_bytes_(std::move(queued_output_bytes)),
      max_output_queue_bytes_(max_output_queue_bytes),
      end_handler_(std::move(handle_source_end))
{
}

void rtmp_play_session::startup()
{
    if (closed_ || !stream_)
    {
        return;
    }
    apply_tracks(std::make_shared<const media_tracks>(stream_->tracks()));
    sink_ = std::make_shared<media_sink>();
    stream_->add_sink(sink_, worker_);
    process_read();
}

void rtmp_play_session::shutdown()
{
    if (closed_)
    {
        return;
    }
    closed_ = true;
    if (sink_)
    {
        sink_->close();
        sink_.reset();
    }
    sink_tracks_.clear();
    waiting_for_key_frame_ = false;
    muxer_.shutdown();
    stream_.reset();
    waiting_for_output_ = false;
}

void rtmp_play_session::on_output_progress()
{
    if (closed_ || !waiting_for_output_ || !output_drained())
    {
        return;
    }
    waiting_for_output_ = false;
    process_read();
}

void rtmp_play_session::process_read()
{
    bool first_entry = true;
    while (!closed_)
    {
        if (!first_entry && output_backpressured())
        {
            waiting_for_output_ = true;
            return;
        }

        auto entry = sink_ ? sink_->read() : std::optional<media_frame>{};
        if (!entry)
        {
            if (sink_ && !waiting_for_output_)
            {
                const auto weak = weak_from_this();
                sink_->async_wait([weak](bool ended)
                                  {
                                      if (const auto self = weak.lock())
                                      {
                                          if (ended && !self->source_ended_)
                                          {
                                              self->source_ended_ = true;
                                              self->end_handler_();
                                          }
                                          else
                                          {
                                              self->process_read();
                                          }
                                      }
                                  });
            }
            return;
        }
        first_entry = false;
        const auto track = sink_tracks_.find(entry->track);
        if (track == sink_tracks_.end())
        {
            continue;
        }

        if (waiting_for_key_frame_)
        {
        if (track->second.kind != media_kind::video || !entry->key_frame)
            {
                continue;
            }
            waiting_for_key_frame_ = false;
        }
        muxer_.on_frame(*entry);
    }
}

std::size_t rtmp_play_session::queued_output_bytes() const
{
    return queued_output_bytes_();
}

// history replay 只使用半个 queue；恢复水位形成滞回，并为单帧展开及 control output 保留余量。
bool rtmp_play_session::output_backpressured() const { return queued_output_bytes() >= max_output_queue_bytes_ / 2U; }

bool rtmp_play_session::output_drained() const { return queued_output_bytes() <= max_output_queue_bytes_ / 4U; }

void rtmp_play_session::apply_tracks(const media_tracks_ptr& tracks)
{
    if (!tracks || !sink_tracks_.empty())
    {
        return;
    }

    for (const auto& track : *tracks)
    {
        sink_tracks_.emplace(track.id, track);
        muxer_.on_track(track);
    }
}

}    // namespace media_server
