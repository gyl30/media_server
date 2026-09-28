#ifndef MEDIA_CORE_MEDIA_HISTORY_IMPL_H
#define MEDIA_CORE_MEDIA_HISTORY_IMPL_H

#include <algorithm>
#include <atomic>
#include <mutex>
#include <utility>

#include <boost/asio/dispatch.hpp>
#include <boost/asio/post.hpp>

#include "media/core/media_history.h"
#include "media/net/worker_context.h"

namespace media_server
{
namespace media_history_detail
{
inline constexpr std::size_t max_gop_frames = 2500;

template <typename Frame>
bool append(std::deque<media_history_entry_t<Frame>>& history,
            std::optional<std::uint64_t>& gop_start,
            std::size_t& gop_frames,
            media_history_entry_t<Frame> entry)
{
    if (entry.kind == media_kind::video && entry.frame.key_frame)
    {
        if (gop_start)
        {
            const auto previous_start = *gop_start;
            std::erase_if(history, [previous_start](const auto& current) { return current.sequence < previous_start; });
        }
        gop_start = entry.sequence;
        gop_frames = 0;
    }
    if (!gop_start && history.empty())
    {
        return false;
    }
    if (gop_frames >= max_gop_frames)
    {
        history.clear();
        gop_start.reset();
        gop_frames = 0;
        return false;
    }
    history.push_back(std::move(entry));
    ++gop_frames;
    return true;
}
}    // namespace media_history_detail

template <typename Frame>
struct media_sink_state_t
{
    std::weak_ptr<media_sink_t<Frame>> sink;
    worker_context* worker{};
    std::atomic_bool active{true};
    // 只由 source owner worker 访问；表示本 state 是否实际计入了 group。
    bool member{};
    std::function<void(bool)> waiter;
};

template <typename Frame>
struct media_worker_history_t : std::enable_shared_from_this<media_worker_history_t<Frame>>
{
    using entry = media_history_entry_t<Frame>;

    struct snapshot
    {
        std::deque<entry> history;
        std::optional<std::uint64_t> gop_start;
        std::size_t gop_frames{};
    };
    worker_context& worker;
    std::weak_ptr<media_history<Frame>> source;
    std::mutex pending_mutex;
    std::deque<entry> pending;
    std::optional<snapshot> pending_snapshot;
    bool pending_end{};
    bool drain_queued{};
    std::size_t source_subscribers{};

    std::deque<entry> history;
    std::optional<std::uint64_t> gop_start;
    std::size_t gop_frames{};
    std::vector<std::shared_ptr<media_sink_state_t<Frame>>> sinks;
    worker_context::shutdown_subscription shutdown_subscription;
    bool ended{};

    media_worker_history_t(worker_context& owner_worker, std::weak_ptr<media_history<Frame>> owner_source)
        : worker(owner_worker), source(std::move(owner_source))
    {
    }

    void retire()
    {
        if (const auto owner = source.lock())
        {
            owner->remove_worker(this->shared_from_this());
        }
    }

    void install(snapshot initial)
    {
        if (worker.stop_requested())
        {
            retire();
            return;
        }
        const auto weak = this->weak_from_this();
        shutdown_subscription = worker.subscribe_shutdown(
            [weak]()
            {
                if (const auto group = weak.lock())
                {
                    group->ended = true;
                    for (const auto& state : group->sinks)
                    {
                        state->active.store(false, std::memory_order_release);
                    }
                    group->sinks.clear();
                    group->history.clear();
                    group->retire();
                }
            });
        history = std::move(initial.history);
        gop_start = initial.gop_start;
        gop_frames = initial.gop_frames;
    }

    void attach(const std::shared_ptr<media_sink_state_t<Frame>>& state)
    {
        if (ended || worker.stop_requested() || !state->active.load(std::memory_order_acquire))
        {
            return;
        }
        const auto sink = state->sink.lock();
        if (!sink)
        {
            return;
        }
        sink->worker_history_ = this->shared_from_this();
        sinks.push_back(state);
        if (state->active.load(std::memory_order_acquire) && !history.empty())
        {
            if (state->waiter)
            {
                auto waiter = std::move(state->waiter);
                waiter(false);
            }
        }
    }

    void detach(const std::shared_ptr<media_sink_state_t<Frame>>& state)
    {
        std::erase(sinks, state);
        if (sinks.empty())
        {
            shutdown_subscription.reset();
            history.clear();
        }
    }

    [[nodiscard]] std::optional<Frame> read(std::optional<std::uint64_t>& cursor)
    {
        if (history.empty())
        {
            return {};
        }
        if (!cursor || *cursor < history.front().sequence)
        {
            if (!gop_start)
            {
                return {};
            }
            cursor = *gop_start;
        }
        const auto it = std::lower_bound(
            history.begin(), history.end(), *cursor, [](const entry& current, std::uint64_t sequence) { return current.sequence < sequence; });
        if (it == history.end())
        {
            return {};
        }
        cursor = it->sequence + 1;
        return it->frame;
    }

    [[nodiscard]] std::size_t pending_size()
    {
        std::scoped_lock lock(pending_mutex);
        return pending.size();
    }

    void enqueue_frame(entry next)
    {
        bool schedule = false;
        {
            std::scoped_lock lock(pending_mutex);
            pending.push_back(std::move(next));
            if (!drain_queued)
            {
                drain_queued = true;
                schedule = true;
            }
        }
        if (schedule)
        {
            const auto self = this->shared_from_this();
            boost::asio::post(worker.io(), [self]() { self->drain(); });
        }
    }

    void replace_pending(snapshot latest)
    {
        bool schedule = false;
        {
            std::scoped_lock lock(pending_mutex);
            pending.clear();
            pending_snapshot = std::move(latest);
            if (!drain_queued)
            {
                drain_queued = true;
                schedule = true;
            }
        }
        if (schedule)
        {
            const auto self = this->shared_from_this();
            boost::asio::post(worker.io(), [self]() { self->drain(); });
        }
    }

    void enqueue_end()
    {
        bool schedule = false;
        {
            std::scoped_lock lock(pending_mutex);
            pending_end = true;
            if (!drain_queued)
            {
                drain_queued = true;
                schedule = true;
            }
        }
        if (schedule)
        {
            const auto self = this->shared_from_this();
            boost::asio::post(worker.io(), [self]() { self->drain(); });
        }
    }

    void notify_waiting()
    {
        for (const auto& state : sinks)
        {
            if (!state->waiter || !state->active.load(std::memory_order_acquire))
            {
                continue;
            }
            if (const auto sink = state->sink.lock())
            {
                static_cast<void>(sink);
                auto waiter = std::move(state->waiter);
                waiter(false);
            }
        }
    }

    void drain()
    {
        std::deque<entry> ready;
        std::optional<snapshot> replacement;
        bool end_requested = false;
        {
            std::scoped_lock lock(pending_mutex);
            ready.swap(pending);
            replacement = std::move(pending_snapshot);
            pending_snapshot.reset();
            end_requested = pending_end;
            pending_end = false;
            drain_queued = false;
        }
        if (ended)
        {
            return;
        }
        bool media_added = false;
        if (replacement)
        {
            history = std::move(replacement->history);
            gop_start = replacement->gop_start;
            gop_frames = replacement->gop_frames;
            media_added = !history.empty();
        }
        for (auto& frame : ready)
        {
            media_added = media_history_detail::append(history, gop_start, gop_frames, std::move(frame)) || media_added;
        }
        if (end_requested)
        {
            if (media_added)
            {
                notify_waiting();
            }
            ended = true;
            for (const auto& state : sinks)
            {
                if (state->active.load(std::memory_order_acquire))
                {
                    if (const auto sink = state->sink.lock())
                    {
                        static_cast<void>(sink);
                        if (state->waiter)
                        {
                            auto waiter = std::move(state->waiter);
                            waiter(true);
                        }
                    }
                }
            }
            shutdown_subscription.reset();
            return;
        }
        if (media_added)
        {
            notify_waiting();
        }
    }
};

template <typename Frame>
media_sink_t<Frame>::~media_sink_t()
{
    close();
}

template <typename Frame>
std::optional<Frame> media_sink_t<Frame>::read()
{
    if (!state_ || !state_->active.load(std::memory_order_acquire) || !worker_history_)
    {
        return {};
    }
    if (auto next = worker_history_->read(cursor_))
    {
        state_->waiter = {};
        return next;
    }
    return {};
}

template <typename Frame>
void media_sink_t<Frame>::close() const
{
    if (!state_ || !state_->active.exchange(false, std::memory_order_acq_rel))
    {
        return;
    }
    if (const auto source = history_.lock())
    {
        source->remove_sink(state_);
    }
}

template <typename Frame>
void media_sink_t<Frame>::async_wait(std::function<void(bool)> handler)
{
    if (!state_ || !state_->active.load(std::memory_order_acquire))
    {
        if (handler)
        {
            handler(true);
        }
        return;
    }
    if (state_->waiter)
    {
        return;
    }
    if (!worker_history_)
    {
        state_->waiter = std::move(handler);
        return;
    }
    if (worker_history_->ended && worker_history_->history.empty())
    {
        handler(true);
        return;
    }
    state_->waiter = std::move(handler);
}

template <typename Frame>
media_history<Frame>::media_history(std::string name, worker_context& worker) : name_(std::move(name)), worker_(worker)
{
}

template <typename Frame>
const std::string& media_history<Frame>::name() const noexcept
{
    return name_;
}

template <typename Frame>
std::vector<media_track> media_history<Frame>::tracks() const
{
    std::vector<media_track> result;
    result.reserve(tracks_.size());
    for (const auto& [id, track] : tracks_)
    {
        result.push_back(track);
    }
    return result;
}

template <typename Frame>
void media_history<Frame>::add_sink(const std::shared_ptr<media_sink_t<Frame>>& sink, worker_context& worker)
{
    if (!sink)
    {
        return;
    }
    const auto self = this->shared_from_this();
    auto state = std::make_shared<media_sink_state_t<Frame>>();
    state->sink = sink;
    state->worker = &worker;
    sink->history_ = self;
    sink->state_ = state;
    boost::asio::dispatch(worker_.io(), [self, state]() { self->add_sink_on_owner(state); });
}

template <typename Frame>
bool media_history<Frame>::set_tracks(std::vector<media_track> tracks)
{
    if (ended_ || !tracks_.empty() || tracks.empty())
    {
        return false;
    }
    std::map<track_id, media_track> initial_tracks;
    for (auto& track : tracks)
    {
        if (track.id == 0)
        {
            return false;
        }
        if (!initial_tracks.emplace(track.id, std::move(track)).second)
        {
            return false;
        }
    }
    tracks_ = std::move(initial_tracks);
    return true;
}

template <typename Frame>
void media_history<Frame>::publish(Frame frame)
{
    if (ended_ || !frame.payload || frame.payload->empty())
    {
        return;
    }
    const auto track = tracks_.find(frame.track);
    if (track == tracks_.end())
    {
        return;
    }
    const auto sequence = next_history_sequence_++;
    media_history_entry entry{.sequence = sequence, .kind = track->second.kind, .frame = frame};
    const bool had_history = !history_.empty();
    if (!media_history_detail::append(history_, current_gop_start_sequence_, current_gop_frames_, entry) && !had_history)
    {
        return;
    }
    for (const auto& [worker, group] : workers_)
    {
        if (group->pending_size() >= media_history_detail::max_gop_frames)
        {
            replace_worker_history(group);
        }
        else
        {
            group->enqueue_frame(entry);
        }
    }
}

template <typename Frame>
void media_history<Frame>::end()
{
    if (ended_)
    {
        return;
    }
    ended_ = true;
    reset_history();
    for (const auto& [worker, group] : workers_)
    {
        group->enqueue_end();
    }
    workers_.clear();
}

template <typename Frame>
std::shared_ptr<media_worker_history_t<Frame>> media_history<Frame>::make_worker_history(worker_context& worker)
{
    auto group = std::make_shared<media_worker_history_t<Frame>>(worker, this->shared_from_this());
    typename media_worker_history_t<Frame>::snapshot initial{
        .history = history_,
        .gop_start = current_gop_start_sequence_,
        .gop_frames = current_gop_frames_};
    boost::asio::post(worker.io(), [group, initial = std::move(initial)]() mutable { group->install(std::move(initial)); });
    return group;
}

template <typename Frame>
void media_history<Frame>::add_sink_on_owner(const std::shared_ptr<media_sink_state_t<Frame>>& state)
{
    if (!state->active.load(std::memory_order_acquire) || state->worker->stop_requested())
    {
        return;
    }
    if (ended_)
    {
        boost::asio::post(state->worker->io(), [state]()
                          {
                              if (state->active.load(std::memory_order_acquire) && state->waiter)
                              {
                                  auto waiter = std::move(state->waiter);
                                  waiter(true);
                              }
                          });
        return;
    }
    auto& group = workers_[state->worker];
    if (!group)
    {
        group = make_worker_history(*state->worker);
    }
    ++group->source_subscribers;
    state->member = true;
    boost::asio::post(state->worker->io(), [group, state]() { group->attach(state); });
}

template <typename Frame>
void media_history<Frame>::remove_sink(const std::shared_ptr<media_sink_state_t<Frame>>& state)
{
    const auto self = this->shared_from_this();
    boost::asio::dispatch(worker_.io(), [self, state]() { self->remove_sink_on_owner(state); });
}

template <typename Frame>
void media_history<Frame>::remove_sink_on_owner(const std::shared_ptr<media_sink_state_t<Frame>>& state)
{
    if (!state->member)
    {
        return;
    }
    state->member = false;
    const auto found = workers_.find(state->worker);
    if (found != workers_.end())
    {
        const auto group = found->second;
        if (--group->source_subscribers == 0)
        {
            workers_.erase(found);
        }
        boost::asio::post(state->worker->io(), [group, state]() { group->detach(state); });
    }
}

template <typename Frame>
void media_history<Frame>::remove_worker(const std::shared_ptr<media_worker_history_t<Frame>>& group)
{
    const auto self = this->shared_from_this();
    boost::asio::dispatch(worker_.io(),
                          [self, group]()
                          {
                              const auto found = self->workers_.find(&group->worker);
                              if (found != self->workers_.end() && found->second == group)
                              {
                                  self->workers_.erase(found);
                              }
                          });
}

template <typename Frame>
void media_history<Frame>::reset_history()
{
    history_.clear();
    current_gop_start_sequence_.reset();
    current_gop_frames_ = 0;
}

template <typename Frame>
void media_history<Frame>::replace_worker_history(const std::shared_ptr<media_worker_history_t<Frame>>& group)
{
    group->replace_pending(typename media_worker_history_t<Frame>::snapshot{
        .history = history_,
        .gop_start = current_gop_start_sequence_,
        .gop_frames = current_gop_frames_});
}

}    // namespace media_server

#endif
