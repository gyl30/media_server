#ifndef MEDIA_CORE_WORKER_SINK_DISPATCHER_H
#define MEDIA_CORE_WORKER_SINK_DISPATCHER_H

#include <algorithm>
#include <cstddef>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include <boost/asio/post.hpp>

#include "media/net/worker_context.h"

namespace media_server
{

template <typename Frame, typename Sink, void (Sink::*Deliver)(const Frame&)>
class worker_sink_dispatcher final
{
   public:
    explicit worker_sink_dispatcher(worker_context& owner) : owner_(owner) {}

    // 业务 owner 保留 add/remove 的入口 dispatch 和终态检查；以下操作都在 owner worker 上执行。
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

    void end()
    {
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
            if (&group->worker == &owner_)
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

    struct sink_group
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
            for (;;)
            {
                std::deque<Frame> frames;
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
                        (sink.get()->*Deliver)(frame);
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

    worker_context& owner_;
    std::map<worker_context*, std::shared_ptr<sink_group>> sink_groups_;
};

}    // namespace media_server

#endif
