#include <utility>

#include <boost/asio/dispatch.hpp>

#include "media/core/media_stream.h"
#include "media/ps/mpeg_ps_output.h"

namespace media_server
{
media_stream::media_stream(std::string name, worker_context& worker) : media_history(std::move(name), worker) {}

worker_context& media_stream::worker() const noexcept { return worker_; }

void media_stream::add_sink(const std::shared_ptr<media_sink>& sink)
{
    if (!sink)
    {
        return;
    }
    const auto self = std::static_pointer_cast<media_stream>(shared_from_this());
    boost::asio::dispatch(worker_.io(),
                          [self, sink]()
                          {
                              if (self->ended_)
                              {
                                  sink->on_end();
                                  return;
                              }
                              if (!self->sink_)
                              {
                                  self->sink_ = sink;
                                  self->replay_to(*sink);
                              }
                          });
}

bool media_stream::set_tracks(std::vector<media_track> tracks)
{
    if (!media_history::set_tracks(std::move(tracks)))
    {
        return false;
    }
    if (sink_)
    {
        for (const auto& [id, track] : tracks_)
        {
            sink_->on_track(track);
        }
    }
    return true;
}

bool media_stream::update_track(media_track track)
{
    const auto id = track.id;
    if (!media_history::update_track(std::move(track)))
    {
        return false;
    }
    sink_replay_barrier_sequence_ = next_history_sequence_;
    if (sink_)
    {
        sink_->on_track(tracks_.at(id));
    }
    if (ps_output_)
    {
        ps_output_->on_track(tracks_.at(id));
    }
    return true;
}

void media_stream::publish(media_frame frame)
{
    if (ended_ || !frame.payload || frame.payload->empty() || !tracks_.contains(frame.track))
    {
        return;
    }
    media_history::publish(frame);
    if (sink_)
    {
        sink_->on_frame(frame);
    }
    if (ps_output_)
    {
        ps_output_->on_frame(frame);
    }
}

void media_stream::end()
{
    if (ended_)
    {
        return;
    }
    media_history::end();
    if (auto sink = std::move(sink_))
    {
        sink->on_end();
    }
    if (ps_output_)
    {
        ps_output_->on_end();
    }
}

void media_stream::replay_to(media_sink& sink)
{
    std::vector<media_frame> frames;
    if (current_gop_start_sequence_ && *current_gop_start_sequence_ >= sink_replay_barrier_sequence_)
    {
        for (const auto& entry : history_)
        {
            if (entry.sequence >= *current_gop_start_sequence_)
            {
                frames.push_back(entry.frame);
            }
        }
    }
    for (const auto& [id, track] : tracks_)
    {
        sink.on_track(track);
    }
    for (const auto& frame : frames)
    {
        sink.on_frame(frame);
    }
}

std::shared_ptr<mpeg_ps_output> media_stream::ps_output()
{
    if (ended_)
    {
        return {};
    }
    if (!ps_output_)
    {
        auto output = std::make_shared<mpeg_ps_output>(name_, worker_);
        if (!output->startup(tracks()))
        {
            return output;
        }
        ps_output_ = std::move(output);
        replay_to(*ps_output_);
    }
    return ps_output_;
}

}    // namespace media_server
