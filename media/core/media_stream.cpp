#include <algorithm>
#include <deque>
#include <mutex>
#include <utility>

#include <boost/asio/dispatch.hpp>
#include <boost/asio/post.hpp>

#include "media/core/media_stream.h"
#include "media/net/worker_context.h"
#include "media/ps/mpeg_ps_output.h"

namespace media_server
{
namespace
{
constexpr std::size_t max_pending_frames = 2500;
}

struct media_stream::sink_group
{
    worker_context& worker;
    std::mutex mutex;
    std::vector<std::weak_ptr<media_sink>> sinks;
    std::deque<media_frame> pending;
    bool drain_queued{};
    bool pending_end{};

    explicit sink_group(worker_context& target) : worker(target) {}

    [[nodiscard]] std::vector<std::shared_ptr<media_sink>> snapshot_sinks()
    {
        std::scoped_lock lock(mutex);
        std::vector<std::shared_ptr<media_sink>> result;
        std::erase_if(sinks, [](const auto& sink) { return sink.expired(); });
        result.reserve(sinks.size());
        for (const auto& weak : sinks)
        {
            if (auto sink = weak.lock())
            {
                result.push_back(std::move(sink));
            }
        }
        return result;
    }

    void drain()
    {
        for (;;)
        {
            std::deque<media_frame> frames;
            bool finish = false;
            {
                std::scoped_lock lock(mutex);
                frames.swap(pending);
                if (frames.empty() && pending_end)
                {
                    pending_end = false;
                    drain_queued = false;
                    finish = true;
                }
                else if (frames.empty())
                {
                    drain_queued = false;
                    return;
                }
            }
            const auto sinks_snapshot = snapshot_sinks();
            for (const auto& frame : frames)
            {
                for (const auto& sink : sinks_snapshot)
                {
                    sink->on_frame(frame);
                }
            }
            if (finish)
            {
                for (const auto& sink : sinks_snapshot)
                {
                    sink->on_end();
                }
                return;
            }
        }
    }
};

media_stream::media_stream(std::string name, worker_context& worker) : name_(std::move(name)), worker_(worker) {}
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
    auto& group = sink_groups_[&worker];
    if (!group)
    {
        group = std::make_shared<sink_group>(worker);
    }
    std::scoped_lock lock(group->mutex);
    group->sinks.emplace_back(std::move(sink));
}

void media_stream::remove_sink(media_sink* sink)
{
    if (!sink)
    {
        return;
    }
    boost::asio::dispatch(worker_.io(), [self = shared_from_this(), sink]() { self->remove_sink_owner(sink); });
}

void media_stream::remove_sink_owner(media_sink* sink)
{
    for (auto iterator = sink_groups_.begin(); iterator != sink_groups_.end();)
    {
        const auto& group = iterator->second;
        {
            std::scoped_lock lock(group->mutex);
            std::erase_if(group->sinks, [sink](const auto& weak)
                          {
                              const auto current = weak.lock();
                              return !current || current.get() == sink;
                          });
        }
        if (group->snapshot_sinks().empty())
        {
            iterator = sink_groups_.erase(iterator);
        }
        else
        {
            ++iterator;
        }
    }
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
    std::vector<std::shared_ptr<sink_group>> groups;
    groups.reserve(sink_groups_.size());
    for (const auto& [worker, group] : sink_groups_)
    {
        static_cast<void>(worker);
        groups.push_back(group);
    }
    std::vector<std::shared_ptr<sink_group>> overflowed_groups;
    for (const auto& group : groups)
    {
        if (&group->worker == &worker_)
        {
            for (const auto& sink : group->snapshot_sinks())
            {
                sink->on_frame(frame);
            }
            continue;
        }
        bool schedule = false;
        {
            std::scoped_lock lock(group->mutex);
            if (group->pending_end)
            {
                continue;
            }
            if (group->pending.size() >= max_pending_frames)
            {
                group->pending.clear();
                group->pending_end = true;
                overflowed_groups.push_back(group);
            }
            else
            {
                group->pending.push_back(frame);
            }
            if (!group->drain_queued)
            {
                group->drain_queued = true;
                schedule = true;
            }
        }
        if (schedule)
        {
            boost::asio::post(group->worker.io(), [group]() { group->drain(); });
        }
    }
    for (const auto& overflowed : overflowed_groups)
    {
        for (auto iterator = sink_groups_.begin(); iterator != sink_groups_.end();)
        {
            if (iterator->second == overflowed)
            {
                iterator = sink_groups_.erase(iterator);
            }
            else
            {
                ++iterator;
            }
        }
    }
}

void media_stream::end_group(const std::shared_ptr<sink_group>& group)
{
    if (&group->worker == &worker_)
    {
        for (const auto& sink : group->snapshot_sinks())
        {
            sink->on_end();
        }
        return;
    }
    bool schedule = false;
    {
        std::scoped_lock lock(group->mutex);
        group->pending_end = true;
        if (!group->drain_queued)
        {
            group->drain_queued = true;
            schedule = true;
        }
    }
    if (schedule)
    {
        boost::asio::post(group->worker.io(), [group]() { group->drain(); });
    }
}

void media_stream::end()
{
    if (ended_)
    {
        return;
    }
    ended_ = true;
    std::vector<std::shared_ptr<sink_group>> groups;
    groups.reserve(sink_groups_.size());
    for (const auto& [worker, group] : sink_groups_)
    {
        static_cast<void>(worker);
        groups.push_back(group);
    }
    sink_groups_.clear();
    for (const auto& group : groups)
    {
        end_group(group);
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
    auto output = std::make_shared<mpeg_ps_output>(worker_);
    if (!output->startup(std::static_pointer_cast<media_stream>(shared_from_this())))
    {
        return {};
    }
    ps_output_ = output;
    return output;
}

}    // namespace media_server
