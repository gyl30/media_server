#ifndef MEDIA_CORE_MEDIA_HISTORY_IMPL_H
#define MEDIA_CORE_MEDIA_HISTORY_IMPL_H

#include <algorithm>
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
struct media_reader_state_t
{
    std::weak_ptr<media_reader_t<Frame>> reader;
    worker_context* worker{};
    std::atomic_bool active{true};
    bool terminal{};
    bool waiting{};
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
    media_tracks_ptr tracks;
    std::vector<std::shared_ptr<media_reader_state_t<Frame>>> readers;
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

    void install(snapshot initial, media_tracks_ptr fixed_tracks)
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
                    for (const auto& state : group->readers)
                    {
                        state->active.store(false, std::memory_order_release);
                    }
                    group->readers.clear();
                    group->history.clear();
                    group->retire();
                }
            });
        history = std::move(initial.history);
        gop_start = initial.gop_start;
        gop_frames = initial.gop_frames;
        tracks = std::move(fixed_tracks);
    }

    void attach(const std::shared_ptr<media_reader_state_t<Frame>>& state)
    {
        if (ended || worker.stop_requested() || !state->active.load(std::memory_order_acquire))
        {
            return;
        }
        const auto reader = state->reader.lock();
        if (!reader)
        {
            return;
        }
        reader->worker_history_ = this->shared_from_this();
        readers.push_back(state);
        if (tracks)
        {
            reader->on_tracks(tracks);
        }
        if (state->active.load(std::memory_order_acquire) && !history.empty())
        {
            reader->on_media_available();
        }
    }

    void detach(const std::shared_ptr<media_reader_state_t<Frame>>& state)
    {
        std::erase(readers, state);
        if (readers.empty())
        {
            shutdown_subscription.reset();
            history.clear();
        }
    }

    [[nodiscard]] std::optional<media_read_entry_t<Frame>> read(std::optional<std::uint64_t>& cursor)
    {
        if (ended || history.empty())
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
        return media_read_entry_t<Frame>{.frame = it->frame};
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
        for (const auto& state : readers)
        {
            if (!state->waiting || !state->active.load(std::memory_order_acquire))
            {
                continue;
            }
            if (const auto reader = state->reader.lock())
            {
                state->waiting = false;
                reader->on_media_available();
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
            for (const auto& state : readers)
            {
                state->terminal = true;
                if (state->active.load(std::memory_order_acquire))
                {
                    if (const auto reader = state->reader.lock())
                    {
                        reader->on_end();
                    }
                }
            }
            readers.clear();
            history.clear();
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
media_reader_t<Frame>::~media_reader_t()
{
    remove_reader();
}

template <typename Frame>
std::optional<media_read_entry_t<Frame>> media_reader_t<Frame>::read()
{
    if (!state_ || !state_->active.load(std::memory_order_acquire) || state_->terminal || !worker_history_)
    {
        return {};
    }
    if (auto next = worker_history_->read(reader_cursor_))
    {
        state_->waiting = false;
        return next;
    }
    state_->waiting = true;
    return {};
}

template <typename Frame>
void media_reader_t<Frame>::remove_reader() const
{
    if (!state_ || !state_->active.exchange(false, std::memory_order_acq_rel))
    {
        return;
    }
    if (const auto source = history_.lock())
    {
        source->remove_reader(state_);
    }
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
    const auto snapshot = track_snapshot_.load(std::memory_order_acquire);
    return snapshot ? *snapshot : std::vector<media_track>{};
}

template <typename Frame>
void media_history<Frame>::add_reader(const std::shared_ptr<media_reader_t<Frame>>& reader, worker_context& worker)
{
    if (!reader)
    {
        return;
    }
    const auto self = this->shared_from_this();
    auto state = std::make_shared<media_reader_state_t<Frame>>();
    state->reader = reader;
    state->worker = &worker;
    reader->history_ = self;
    reader->state_ = state;
    boost::asio::dispatch(worker_.io(), [self, state]() { self->add_reader_on_owner(state); });
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
    auto snapshot = std::make_shared<media_tracks>();
    snapshot->reserve(tracks_.size());
    for (const auto& [id, track] : tracks_)
    {
        snapshot->push_back(track);
    }
    track_snapshot_.store(std::move(snapshot), std::memory_order_release);
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
    const auto tracks = track_snapshot_.load(std::memory_order_acquire);
    typename media_worker_history_t<Frame>::snapshot initial{
        .history = history_,
        .gop_start = current_gop_start_sequence_,
        .gop_frames = current_gop_frames_};
    boost::asio::post(worker.io(), [group, tracks, initial = std::move(initial)]() mutable { group->install(std::move(initial), tracks); });
    return group;
}

template <typename Frame>
void media_history<Frame>::add_reader_on_owner(const std::shared_ptr<media_reader_state_t<Frame>>& state)
{
    if (!state->active.load(std::memory_order_acquire) || state->worker->stop_requested())
    {
        return;
    }
    if (ended_)
    {
        dispatch_reader_end(state);
        return;
    }
    auto& group = workers_[state->worker];
    if (!group)
    {
        group = make_worker_history(*state->worker);
    }
    ++group->source_subscribers;
    boost::asio::post(state->worker->io(), [group, state]() { group->attach(state); });
}

template <typename Frame>
void media_history<Frame>::remove_reader(const std::shared_ptr<media_reader_state_t<Frame>>& state)
{
    const auto self = this->shared_from_this();
    boost::asio::dispatch(worker_.io(), [self, state]() { self->remove_reader_on_owner(state); });
}

template <typename Frame>
void media_history<Frame>::remove_reader_on_owner(const std::shared_ptr<media_reader_state_t<Frame>>& state)
{
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

template <typename Frame>
void media_history<Frame>::dispatch_reader_end(const std::shared_ptr<media_reader_state_t<Frame>>& state)
{
    boost::asio::post(state->worker->io(),
                      [state]()
                      {
                          state->terminal = true;
                          if (state->active.load(std::memory_order_acquire))
                          {
                              if (const auto reader = state->reader.lock())
                              {
                                  reader->on_end();
                              }
                          }
                      });
}
}    // namespace media_server

#endif
