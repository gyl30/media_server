#include <algorithm>
#include <map>
#include <utility>

#include <boost/asio/dispatch.hpp>
#include <boost/asio/post.hpp>

#include "media/core/media_stream.h"
#include "media/net/worker_context.h"
#include "media/ps/mpeg_ps_output.h"

namespace media_server
{
media_stream::media_stream(std::string name, worker_context& worker) : name_(std::move(name)), worker_(worker), dispatcher_(worker) {}
media_stream::~media_stream() = default;

const std::string& media_stream::name() const noexcept { return name_; }
worker_context& media_stream::worker() const noexcept { return worker_; }
const std::vector<media_track>& media_stream::tracks() const noexcept { return tracks_; }

bool media_stream::set_tracks(std::vector<media_track> tracks)
{
    if (ended_ || !tracks_.empty() || tracks.empty())
    {
        return false;
    }
    std::map<track_id, bool> ids;
    for (const auto& track : tracks)
    {
        if (track.id == 0 || !ids.emplace(track.id, true).second)
        {
            return false;
        }
    }
    tracks_ = std::move(tracks);
    return true;
}

void media_stream::add_sink(std::shared_ptr<media_sink> sink)
{
    if (!sink)
    {
        return;
    }
    auto* target = &sink->worker();
    boost::asio::dispatch(worker_.io(), [self = shared_from_this(), sink = std::move(sink), target]() mutable
                          { self->add_sink_owner(std::move(sink), *target); });
}

void media_stream::add_sink_owner(std::shared_ptr<media_sink> sink, worker_context& worker)
{
    if (ended_)
    {
        boost::asio::post(worker.io(), [sink = std::move(sink)]() { sink->on_end(); });
        return;
    }
    dispatcher_.add(std::move(sink), worker);
}

void media_stream::remove_sink(media_sink* sink)
{
    if (!sink)
    {
        return;
    }
    boost::asio::dispatch(worker_.io(), [self = shared_from_this(), sink]() { self->dispatcher_.remove(sink); });
}

void media_stream::publish(media_frame frame)
{
    if (ended_ || !frame.payload || frame.payload->empty())
    {
        return;
    }
    const auto track = std::find_if(tracks_.begin(), tracks_.end(), [&frame](const auto& value) { return value.id == frame.track; });
    if (track == tracks_.end())
    {
        return;
    }
    dispatcher_.publish(frame);
}

void media_stream::end()
{
    if (ended_)
    {
        return;
    }
    ended_ = true;
    dispatcher_.end();
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
    auto output = std::make_shared<mpeg_ps_output>(worker_);
    if (!output->startup(std::static_pointer_cast<media_stream>(shared_from_this())))
    {
        return {};
    }
    ps_output_ = output;
    return output;
}

}    // namespace media_server
