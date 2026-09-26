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
    boost::asio::dispatch(worker_.io(), [self, sink]() { self->attach_sink(sink); });
}

void media_stream::attach_sink(std::shared_ptr<media_sink> sink)
{
    if (ended_)
    {
        sink->on_end();
        return;
    }
    sinks_.push_back(std::move(sink));
    replay_to(*sinks_.back());
}

bool media_stream::set_tracks(std::vector<media_track> tracks)
{
    if (!media_history::set_tracks(std::move(tracks)))
    {
        return false;
    }
    for (const auto& sink : sinks_)
    {
        for (const auto& [id, track] : tracks_)
        {
            sink->on_track(track);
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
    for (const auto& sink : sinks_)
    {
        sink->on_track(tracks_.at(id));
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
    for (const auto& sink : sinks_)
    {
        sink->on_frame(frame);
    }
}

void media_stream::end()
{
    if (ended_)
    {
        return;
    }
    media_history::end();
    auto sinks = std::move(sinks_);
    for (const auto& sink : sinks)
    {
        sink->on_end();
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
    if (const auto output = ps_output_.lock())
    {
        return output;
    }
    auto output = std::make_shared<mpeg_ps_output>(name_, worker_);
    if (!output->startup(tracks()))
    {
        return output;
    }
    ps_output_ = output;
    attach_sink(output);
    return output;
}

}    // namespace media_server
