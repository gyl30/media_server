#include "media/core/media_stream.h"

#include <boost/asio/dispatch.hpp>
#include <boost/asio/post.hpp>

#include <algorithm>
#include <utility>

namespace media_server
{
namespace
{
constexpr std::size_t max_gop_cache_frames = 2500;
}

media_stream::media_stream(std::string name, boost::asio::any_io_executor owner_executor)
    : name_(std::move(name)), owner_executor_(std::move(owner_executor))
{
}

const std::string& media_stream::name() const noexcept { return name_; }

bool media_stream::ended() const noexcept { return ended_.load(std::memory_order_acquire); }

std::vector<media_track> media_stream::tracks() const
{
    const auto snapshot = track_snapshot_.load(std::memory_order_acquire);
    return snapshot ? *snapshot : std::vector<media_track>{};
}

void media_stream::add_sink(const std::shared_ptr<media_sink>& sink, boost::asio::any_io_executor executor)
{
    if (!sink)
    {
        return;
    }
    if (!owner_executor_)
    {
        add_sink_on_owner(sink, std::move(executor));
        return;
    }

    const auto self = weak_from_this().lock();
    if (!self)
    {
        return;
    }
    const std::weak_ptr<media_sink> weak_sink = sink;
    boost::asio::dispatch(owner_executor_,
                          [self, weak_sink, executor = std::move(executor)]() mutable
                      {
                          if (auto current = weak_sink.lock())
                          {
                              self->add_sink_on_owner(std::move(current), std::move(executor));
                          }
                      });
}

void media_stream::remove_sink(const media_sink& sink)
{
    const auto snapshot = sink_snapshot_.load(std::memory_order_acquire);
    if (snapshot)
    {
        for (const auto& state : *snapshot)
        {
            const auto current = state->sink.lock();
            if (current && current.get() == &sink)
            {
                state->active.store(false, std::memory_order_release);
            }
        }
    }

    if (!owner_executor_)
    {
        remove_sink_on_owner(&sink);
        return;
    }

    const auto self = weak_from_this().lock();
    if (!self)
    {
        return;
    }
    const auto* sink_address = &sink;
    boost::asio::dispatch(owner_executor_, [self, sink_address]() { self->remove_sink_on_owner(sink_address); });
}

bool media_stream::update_track(media_track track)
{
    if (ended() || track.id == 0 || track.codec_config.empty())
    {
        return false;
    }

    const auto existing = tracks_.find(track.id);
    if (existing != tracks_.end())
    {
        if (existing->second.kind != track.kind || existing->second.codec != track.codec)
        {
            return false;
        }
        if (existing->second.clock_rate == track.clock_rate && existing->second.channel_count == track.channel_count &&
            existing->second.codec_config == track.codec_config)
        {
            return false;
        }
        track.config_version = existing->second.config_version + 1;
    }
    else
    {
        track.config_version = 1;
    }

    const auto id = track.id;
    gop_cache_.clear();
    tracks_.insert_or_assign(id, std::move(track));
    publish_track_snapshot();
    dispatch_track(tracks_.at(id));
    return true;
}

void media_stream::publish(media_frame frame)
{
    if (ended() || !frame.payload || frame.payload->empty())
    {
        return;
    }

    const auto track = tracks_.find(frame.track);
    if (track == tracks_.end())
    {
        return;
    }

    if (track->second.kind == media_kind::video && frame.key_frame)
    {
        gop_cache_.clear();
        gop_cache_.push_back(frame);
    }
    else if (!gop_cache_.empty())
    {
        if (gop_cache_.size() >= max_gop_cache_frames)
        {
            gop_cache_.clear();
        }
        else
        {
            gop_cache_.push_back(frame);
        }
    }

    dispatch_frame(frame);
}

void media_stream::end()
{
    if (ended_.exchange(true, std::memory_order_acq_rel))
    {
        return;
    }

    gop_cache_.clear();
    remove_inactive_sinks();

    struct end_group
    {
        boost::asio::any_io_executor executor;
        std::vector<std::shared_ptr<media_sink>> sinks;
    };

    std::vector<end_group> groups;
    groups.reserve(sink_groups_.size());
    for (const auto& group : sink_groups_)
    {
        end_group current{.executor = group.executor, .sinks = {}};
        current.sinks.reserve(group.sinks->size());
        for (const auto& state : *group.sinks)
        {
            if (!state->active.load(std::memory_order_acquire))
            {
                continue;
            }
            if (auto sink = state->sink.lock())
            {
                current.sinks.push_back(std::move(sink));
            }
        }
        if (!current.sinks.empty())
        {
            groups.push_back(std::move(current));
        }
    }

    sink_groups_.clear();
    publish_sink_snapshot();

    for (auto& group : groups)
    {
        if (local_executor(group.executor))
        {
            for (const auto& sink : group.sinks)
            {
                sink->on_end();
            }
            continue;
        }
        boost::asio::post(group.executor,
                          [sinks = std::move(group.sinks)]()
                          {
                              for (const auto& sink : sinks)
                              {
                                  sink->on_end();
                              }
                          });
    }
}

void media_stream::add_sink_on_owner(std::shared_ptr<media_sink> sink, boost::asio::any_io_executor executor)
{
    if (ended())
    {
        if (local_executor(executor))
        {
            sink->on_end();
        }
        else
        {
            boost::asio::post(executor, [sink = std::move(sink)]() { sink->on_end(); });
        }
        return;
    }

    remove_inactive_sinks();
    if (has_sink(*sink))
    {
        return;
    }

    auto state = std::make_shared<sink_state>();
    state->sink = sink;
    state->executor = executor;

    auto group = std::find_if(sink_groups_.begin(), sink_groups_.end(), [&executor](const sink_group& value) { return value.executor == executor; });
    if (group == sink_groups_.end())
    {
        sink_groups_.push_back(sink_group{
            .executor = executor,
            .sinks = std::make_shared<const std::vector<std::shared_ptr<sink_state>>>(),
        });
        group = std::prev(sink_groups_.end());
    }

    auto sinks = std::make_shared<std::vector<std::shared_ptr<sink_state>>>(*group->sinks);
    sinks->push_back(state);
    group->sinks = std::move(sinks);
    publish_sink_snapshot();

    replay_sink(state, tracks(), gop_cache_);
}

void media_stream::remove_sink_on_owner(const media_sink* sink)
{
    if (sink != nullptr)
    {
        for (const auto& group : sink_groups_)
        {
            for (const auto& state : *group.sinks)
            {
                const auto current = state->sink.lock();
                if (current && current.get() == sink)
                {
                    state->active.store(false, std::memory_order_release);
                }
            }
        }
    }
    remove_inactive_sinks();
}

void media_stream::remove_inactive_sinks()
{
    bool changed = false;
    for (auto& group : sink_groups_)
    {
        auto sinks = std::make_shared<std::vector<std::shared_ptr<sink_state>>>(*group.sinks);
        const auto old_size = sinks->size();
        std::erase_if(*sinks,
                      [](const std::shared_ptr<sink_state>& state)
                      {
                          if (!state->active.load(std::memory_order_acquire))
                          {
                              return true;
                          }
                          if (state->sink.expired())
                          {
                              state->active.store(false, std::memory_order_release);
                              return true;
                          }
                          return false;
                      });
        if (sinks->size() != old_size)
        {
            group.sinks = std::move(sinks);
            changed = true;
        }
    }

    const auto old_size = sink_groups_.size();
    std::erase_if(sink_groups_, [](const sink_group& group) { return group.sinks->empty(); });
    changed = changed || sink_groups_.size() != old_size;
    if (changed)
    {
        publish_sink_snapshot();
    }
}

void media_stream::publish_sink_snapshot()
{
    std::vector<std::shared_ptr<sink_state>> states;
    for (const auto& group : sink_groups_)
    {
        states.insert(states.end(), group.sinks->begin(), group.sinks->end());
    }
    sink_snapshot_.store(std::make_shared<const std::vector<std::shared_ptr<sink_state>>>(std::move(states)), std::memory_order_release);
}

void media_stream::publish_track_snapshot()
{
    std::vector<media_track> snapshot;
    snapshot.reserve(tracks_.size());
    for (const auto& [id, track] : tracks_)
    {
        static_cast<void>(id);
        snapshot.push_back(track);
    }
    track_snapshot_.store(std::make_shared<const std::vector<media_track>>(std::move(snapshot)), std::memory_order_release);
}

bool media_stream::has_sink(const media_sink& sink) const
{
    for (const auto& group : sink_groups_)
    {
        for (const auto& state : *group.sinks)
        {
            if (!state->active.load(std::memory_order_acquire))
            {
                continue;
            }
            const auto current = state->sink.lock();
            if (current && current.get() == &sink)
            {
                return true;
            }
        }
    }
    return false;
}

bool media_stream::local_executor(const boost::asio::any_io_executor& executor) const
{
    return !owner_executor_ || !executor || executor == owner_executor_;
}

void media_stream::replay_sink(const std::shared_ptr<sink_state>& state,
                               std::vector<media_track> tracks,
                               std::vector<media_frame> frames)
{
    if (local_executor(state->executor))
    {
        const auto sink = state->sink.lock();
        if (!sink)
        {
            return;
        }
        for (const auto& track : tracks)
        {
            if (ended() || !state->active.load(std::memory_order_acquire))
            {
                return;
            }
            const auto current = tracks_.find(track.id);
            if (current == tracks_.end() || current->second.config_version != track.config_version)
            {
                continue;
            }
            sink->on_track(current->second);
        }
        for (const auto& frame : frames)
        {
            if (ended() || !state->active.load(std::memory_order_acquire))
            {
                return;
            }
            sink->on_frame(frame);
        }
        return;
    }

    boost::asio::post(state->executor,
                      [state, tracks = std::move(tracks), frames = std::move(frames)]()
                      {
                          const auto sink = state->sink.lock();
                          if (!sink)
                          {
                              return;
                          }
                          for (const auto& track : tracks)
                          {
                              if (!state->active.load(std::memory_order_acquire))
                              {
                                  return;
                              }
                              sink->on_track(track);
                          }
                          for (const auto& frame : frames)
                          {
                              if (!state->active.load(std::memory_order_acquire))
                              {
                                  return;
                              }
                              sink->on_frame(frame);
                          }
                      });
}

void media_stream::dispatch_track(const media_track& track)
{
    for (const auto& group : sink_groups_)
    {
        if (local_executor(group.executor))
        {
            continue;
        }
        const auto sinks = group.sinks;
        boost::asio::post(group.executor,
                          [sinks, track]()
                          {
                              for (const auto& state : *sinks)
                              {
                                  if (!state->active.load(std::memory_order_acquire))
                                  {
                                      continue;
                                  }
                                  if (const auto sink = state->sink.lock())
                                  {
                                      sink->on_track(track);
                                  }
                              }
                          });
    }

    const auto id = track.id;
    const auto config_version = track.config_version;
    const auto snapshot = sink_snapshot_.load(std::memory_order_acquire);
    if (!snapshot)
    {
        return;
    }
    for (const auto& state : *snapshot)
    {
        if (!local_executor(state->executor) || !state->active.load(std::memory_order_acquire))
        {
            continue;
        }
        const auto current = tracks_.find(id);
        if (current == tracks_.end() || current->second.config_version != config_version)
        {
            return;
        }
        if (const auto sink = state->sink.lock())
        {
            sink->on_track(current->second);
            if (ended())
            {
                return;
            }
        }
    }
}

void media_stream::dispatch_frame(const media_frame& frame)
{
    for (const auto& group : sink_groups_)
    {
        if (local_executor(group.executor))
        {
            continue;
        }
        const auto sinks = group.sinks;
        boost::asio::post(group.executor,
                          [sinks, frame]()
                          {
                              for (const auto& state : *sinks)
                              {
                                  if (!state->active.load(std::memory_order_acquire))
                                  {
                                      continue;
                                  }
                                  if (const auto sink = state->sink.lock())
                                  {
                                      sink->on_frame(frame);
                                  }
                              }
                          });
    }

    const auto snapshot = sink_snapshot_.load(std::memory_order_acquire);
    if (!snapshot)
    {
        return;
    }
    for (const auto& state : *snapshot)
    {
        if (!local_executor(state->executor) || !state->active.load(std::memory_order_acquire))
        {
            continue;
        }
        if (const auto sink = state->sink.lock())
        {
            sink->on_frame(frame);
            if (ended())
            {
                return;
            }
        }
    }
}

}    // namespace media_server
