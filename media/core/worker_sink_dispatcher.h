#ifndef MEDIA_CORE_WORKER_SINK_DISPATCHER_H
#define MEDIA_CORE_WORKER_SINK_DISPATCHER_H

#include <map>
#include <deque>
#include <mutex>
#include <chrono>
#include <memory>
#include <vector>
#include <cstddef>
#include <utility>
#include <optional>
#include <algorithm>

#include <boost/asio/post.hpp>

#include "media/net/worker_context.h"

namespace media_server
{

template <typename Frame, typename Sink, void (Sink::*Deliver)(const Frame&)>
class worker_sink_dispatcher final
{
   public:
    explicit worker_sink_dispatcher(worker_context& owner) : owner_(owner) {}

    void add(std::shared_ptr<Sink> sink, worker_context& target)
    {
        auto& group = sink_groups_[&target];
        if (!group)
        {
            group = std::make_shared<sink_group>(target);
        }
        std::scoped_lock lock(group->mutex);
        group->sinks.emplace_back(std::move(sink));
    }

    void remove(Sink* sink)
    {
        for (auto iterator = sink_groups_.begin(); iterator != sink_groups_.end();)
        {
            const auto& group = iterator->second;
            bool group_empty;
            {
                std::scoped_lock lock(group->mutex);
                std::erase_if(group->sinks,
                              [sink](const auto& weak)
                              {
                                  const auto current = weak.lock();
                                  return !current || current.get() == sink;
                              });
                group_empty = group->sinks.empty();
            }
            if (group_empty)
            {
                iterator = sink_groups_.erase(iterator);
            }
            else
            {
                ++iterator;
            }
        }
    }

    void publish(const Frame& frame)
    {
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
            if (&group->worker == &owner_)
            {
                for (const auto& sink : group->snapshot_sinks())
                {
                    (sink.get()->*Deliver)(frame);
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
            const auto iterator = sink_groups_.find(&overflowed->worker);
            if (iterator != sink_groups_.end() && iterator->second == overflowed)
            {
                sink_groups_.erase(iterator);
            }
        }
    }

    void end()
    {
        decltype(sink_groups_) groups;
        groups.swap(sink_groups_);
        for (const auto& [worker, group] : groups)
        {
            if (worker == &owner_)
            {
                for (const auto& sink : group->snapshot_sinks())
                {
                    sink->on_end();
                }
                continue;
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
    }

   private:
    static constexpr std::size_t max_pending_frames = 2500;
    static constexpr auto drain_time_slice = std::chrono::milliseconds{20};

    struct sink_group : std::enable_shared_from_this<sink_group>
    {
        worker_context& worker;
        std::mutex mutex;
        std::vector<std::weak_ptr<Sink>> sinks;
        std::deque<Frame> pending;
        bool drain_queued{};
        bool pending_end{};

        explicit sink_group(worker_context& target) : worker(target) {}

        [[nodiscard]] std::vector<std::shared_ptr<Sink>> snapshot_sinks()
        {
            std::scoped_lock lock(mutex);
            std::vector<std::shared_ptr<Sink>> result;
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
            const auto deadline = std::chrono::steady_clock::now() + drain_time_slice;
            for (;;)
            {
                std::optional<Frame> frame;
                {
                    std::scoped_lock lock(mutex);
                    if (!pending.empty())
                    {
                        frame = std::move(pending.front());
                        pending.pop_front();
                    }
                    else if (pending_end)
                    {
                        pending_end = false;
                        drain_queued = false;
                    }
                    else
                    {
                        drain_queued = false;
                        return;
                    }
                }
                const auto sinks_snapshot = snapshot_sinks();
                if (!frame)
                {
                    for (const auto& sink : sinks_snapshot)
                    {
                        sink->on_end();
                    }
                    return;
                }
                for (const auto& sink : sinks_snapshot)
                {
                    (sink.get()->*Deliver)(*frame);
                }
                if (std::chrono::steady_clock::now() >= deadline)
                {
                    boost::asio::post(worker.io(), [self = this->shared_from_this()]() { self->drain(); });
                    return;
                }
            }
        }
    };

    worker_context& owner_;
    std::map<worker_context*, std::shared_ptr<sink_group>> sink_groups_;
};

}    // namespace media_server

#endif
