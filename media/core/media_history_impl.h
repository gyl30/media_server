#ifndef MEDIA_CORE_MEDIA_HISTORY_IMPL_H
#define MEDIA_CORE_MEDIA_HISTORY_IMPL_H

#include <utility>
#include <algorithm>

#include <boost/asio/post.hpp>
#include <boost/asio/dispatch.hpp>

#include "media/core/media_history.h"
#include "media/net/worker_context.h"

namespace media_server
{
template <typename Frame>
struct media_reader_state_t
{
    std::weak_ptr<media_reader_t<Frame>> reader;
    worker_context* worker{};
    std::atomic_bool active{true};
    std::atomic_bool terminal{};
    std::atomic_bool read_outstanding{};
    media_reader_cursor pending_cursor;
    bool pending_read{};
    bool registered{};
};

namespace media_history_detail
{
inline constexpr std::size_t max_gop_frames = 2500;
inline constexpr std::size_t max_read_batch_entries = 128;

template <typename Frame>
void release_read_outstanding(const std::shared_ptr<media_reader_state_t<Frame>>& state)
{
    state->read_outstanding.store(false, std::memory_order_release);
}
}    // namespace media_history_detail

template <typename Frame>
media_reader_handle_t<Frame>::media_reader_handle_t(std::weak_ptr<media_history<Frame>> stream, std::shared_ptr<media_reader_state_t<Frame>> state)
    : stream_(std::move(stream)), state_(std::move(state))
{
}

template <typename Frame>
void media_reader_handle_t<Frame>::async_read(media_reader_cursor cursor) const
{
    if (!state_ || !state_->active.load(std::memory_order_acquire) || state_->terminal.load(std::memory_order_acquire))
    {
        return;
    }

    bool expected = false;
    if (!state_->read_outstanding.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
    {
        return;
    }
    if (!state_->active.load(std::memory_order_acquire) || state_->terminal.load(std::memory_order_acquire))
    {
        media_history_detail::release_read_outstanding(state_);
        return;
    }
    if (const auto stream = stream_.lock())
    {
        stream->request_read(state_, cursor);
        return;
    }
    media_history_detail::release_read_outstanding(state_);
}

template <typename Frame>
void media_reader_handle_t<Frame>::remove() const
{
    if (!state_ || !state_->active.exchange(false, std::memory_order_acq_rel))
    {
        return;
    }

    state_->read_outstanding.store(false, std::memory_order_release);
    if (const auto stream = stream_.lock())
    {
        stream->remove_reader(state_);
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
    return snapshot ? snapshot->tracks : std::vector<media_track>{};
}

template <typename Frame>
media_reader_handle_t<Frame> media_history<Frame>::add_reader(const std::shared_ptr<media_reader_t<Frame>>& reader, worker_context& worker)
{
    if (!reader)
    {
        return {};
    }
    const auto self = this->shared_from_this();

    auto state = std::make_shared<media_reader_state_t<Frame>>();
    state->reader = reader;
    state->worker = &worker;

    media_reader_handle_t<Frame> handle(self, state);
    reader->handle_ = handle;

    boost::asio::dispatch(worker_.io(), [self, state]() { self->add_reader_on_owner(state); });
    return handle;
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
        track.config_version = 1;
        if (!initial_tracks.emplace(track.id, std::move(track)).second)
        {
            return false;
        }
    }

    tracks_ = std::move(initial_tracks);
    publish_track_snapshot();
    dispatch_reader_tracks(track_snapshot_.load(std::memory_order_acquire));
    return true;
}

template <typename Frame>
bool media_history<Frame>::update_track(media_track track)
{
    if (ended_ || track.id == 0)
    {
        return false;
    }

    const auto existing = tracks_.find(track.id);
    if (existing == tracks_.end() || existing->second.kind != track.kind || existing->second.codec != track.codec)
    {
        return false;
    }
    if (existing->second.clock_rate == track.clock_rate && existing->second.channel_count == track.channel_count &&
        existing->second.codec_config == track.codec_config)
    {
        return false;
    }
    track.config_version = existing->second.config_version + 1;

    const auto id = track.id;
    tracks_.insert_or_assign(id, std::move(track));
    publish_track_snapshot();
    if (tracks_.at(id).kind == media_kind::video)
    {
        current_gop_start_sequence_.reset();
        current_gop_frames_ = 0;
    }
    dispatch_reader_tracks(track_snapshot_.load(std::memory_order_acquire));
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
    append_history(sequence, frame, track->second);
    dispatch_pending_readers();
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
    end_readers();
}

template <typename Frame>
void media_history<Frame>::publish_track_snapshot()
{
    auto snapshot = std::make_shared<media_track_snapshot>();
    snapshot->revision = ++track_revision_;
    snapshot->tracks.reserve(tracks_.size());
    for (const auto& [id, track] : tracks_)
    {
        snapshot->tracks.push_back(track);
    }
    track_snapshot_.store(std::move(snapshot), std::memory_order_release);
}

template <typename Frame>
void media_history<Frame>::request_read(const std::shared_ptr<media_reader_state_t<Frame>>& state, media_reader_cursor cursor)
{
    if (!state || !state->active.load(std::memory_order_acquire) || state->terminal.load(std::memory_order_acquire))
    {
        if (state)
        {
            media_history_detail::release_read_outstanding(state);
        }
        return;
    }
    const auto self = this->shared_from_this();
    boost::asio::dispatch(worker_.io(), [self, state, cursor]() { self->request_read_on_owner(state, cursor); });
}

template <typename Frame>
void media_history<Frame>::remove_reader(const std::shared_ptr<media_reader_state_t<Frame>>& state)
{
    if (!state)
    {
        return;
    }
    const auto self = this->shared_from_this();
    boost::asio::dispatch(worker_.io(), [self, state]() { self->remove_reader_on_owner(state); });
}

template <typename Frame>
void media_history<Frame>::add_reader_on_owner(const std::shared_ptr<media_reader_state_t<Frame>>& state)
{
    if (!state->active.load(std::memory_order_acquire))
    {
        return;
    }
    if (ended_)
    {
        state->terminal.store(true, std::memory_order_release);
        dispatch_reader_end(state);
        return;
    }
    if (state->reader.expired())
    {
        state->active.store(false, std::memory_order_release);
        return;
    }

    remove_inactive_readers();
    state->registered = true;
    readers_.push_back(state);
    dispatch_reader_tracks(state, track_snapshot_.load(std::memory_order_acquire));
}

template <typename Frame>
void media_history<Frame>::request_read_on_owner(const std::shared_ptr<media_reader_state_t<Frame>>& state, media_reader_cursor cursor)
{
    if (ended_ || !state->registered || state->pending_read || !state->active.load(std::memory_order_acquire))
    {
        media_history_detail::release_read_outstanding(state);
        return;
    }

    state->pending_read = true;
    state->pending_cursor = cursor;
    complete_reader_from_history(state, false);
}

template <typename Frame>
void media_history<Frame>::remove_reader_on_owner(const std::shared_ptr<media_reader_state_t<Frame>>& state)
{
    state->registered = false;
    state->pending_read = false;
    state->read_outstanding.store(false, std::memory_order_release);
    std::erase(readers_, state);
}

template <typename Frame>
void media_history<Frame>::remove_inactive_readers()
{
    std::erase_if(readers_,
                  [](const std::shared_ptr<media_reader_state_t<Frame>>& state)
                  {
                      if (state->active.load(std::memory_order_acquire) && !state->reader.expired())
                      {
                          return false;
                      }
                      state->registered = false;
                      state->pending_read = false;
                      state->read_outstanding.store(false, std::memory_order_release);
                      return true;
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
void media_history<Frame>::dispatch_reader_tracks(const media_track_snapshot_ptr& tracks)
{
    if (!tracks)
    {
        return;
    }

    remove_inactive_readers();
    for (const auto& state : readers_)
    {
        dispatch_reader_tracks(state, tracks);
    }
}

template <typename Frame>
void media_history<Frame>::end_readers()
{
    remove_inactive_readers();
    auto readers = std::move(readers_);
    readers_.clear();
    for (const auto& state : readers)
    {
        state->terminal.store(true, std::memory_order_release);
        state->read_outstanding.store(false, std::memory_order_release);
        state->registered = false;
        state->pending_read = false;
        dispatch_reader_end(state);
    }
}

template <typename Frame>
void media_history<Frame>::append_history(std::uint64_t sequence, const Frame& frame, const media_track& track)
{
    if (track.kind == media_kind::video && frame.key_frame)
    {
        if (current_gop_start_sequence_)
        {
            const auto previous_start = *current_gop_start_sequence_;
            std::erase_if(history_, [previous_start](const media_history_entry& entry) { return entry.sequence < previous_start; });
        }
        current_gop_start_sequence_ = sequence;
        current_gop_frames_ = 0;
    }

    if (!current_gop_start_sequence_ && history_.empty())
    {
        return;
    }
    if (current_gop_frames_ >= media_history_detail::max_gop_frames)
    {
        reset_history();
        return;
    }

    history_.push_back(media_history_entry{.sequence = sequence, .config_version = track.config_version, .frame = frame});
    ++current_gop_frames_;
}

template <typename Frame>
void media_history<Frame>::dispatch_pending_readers()
{
    remove_inactive_readers();
    for (const auto& state : readers_)
    {
        if (state->pending_read && state->active.load(std::memory_order_acquire))
        {
            complete_reader_from_history(state, true);
        }
    }
}

template <typename Frame>
void media_history<Frame>::complete_reader_from_history(const std::shared_ptr<media_reader_state_t<Frame>>& state, bool waited_for_media)
{
    if (!state->pending_read || history_.empty())
    {
        return;
    }

    auto cursor = state->pending_cursor;
    const auto first_sequence = history_.front().sequence;
    if (!cursor || *cursor < first_sequence)
    {
        if (!current_gop_start_sequence_)
        {
            return;
        }
        cursor = *current_gop_start_sequence_;
    }
    if (*cursor > history_.back().sequence)
    {
        return;
    }

    auto iterator = std::ranges::find_if(history_, [cursor](const media_history_entry& entry) { return entry.sequence >= *cursor; });
    if (iterator == history_.end())
    {
        return;
    }

    media_read_batch_t<Frame> batch;
    batch.tracks = track_snapshot_.load(std::memory_order_acquire);
    batch.waited_for_media = waited_for_media;
    batch.entries.reserve(media_history_detail::max_read_batch_entries);
    for (; iterator != history_.end() && batch.entries.size() < media_history_detail::max_read_batch_entries; ++iterator)
    {
        batch.next_cursor = iterator->sequence + 1;
        batch.entries.push_back(media_read_entry_t<Frame>{.config_version = iterator->config_version, .frame = iterator->frame});
    }

    if (batch.entries.empty())
    {
        return;
    }

    state->pending_read = false;
    deliver_reader_batch(state, std::move(batch));
}

template <typename Frame>
void media_history<Frame>::deliver_reader_batch(const std::shared_ptr<media_reader_state_t<Frame>>& state, media_read_batch_t<Frame> batch)
{
    if (!state->active.load(std::memory_order_acquire))
    {
        media_history_detail::release_read_outstanding(state);
        return;
    }

    boost::asio::post(state->worker->io(),
                      [state, batch = std::move(batch)]() mutable
                      {
                          if (!state->active.load(std::memory_order_acquire) || state->terminal.load(std::memory_order_acquire))
                          {
                              media_history_detail::release_read_outstanding(state);
                              return;
                          }
                          if (const auto reader = state->reader.lock())
                          {
                              state->read_outstanding.store(false, std::memory_order_release);
                              reader->on_read(std::move(batch));
                              return;
                          }
                          media_history_detail::release_read_outstanding(state);
                      });
}

template <typename Frame>
void media_history<Frame>::dispatch_reader_tracks(const std::shared_ptr<media_reader_state_t<Frame>>& state, media_track_snapshot_ptr tracks)
{
    if (!tracks)
    {
        return;
    }

    boost::asio::post(state->worker->io(),
                      [state, tracks = std::move(tracks)]()
                      {
                          if (!state->active.load(std::memory_order_acquire) || state->terminal.load(std::memory_order_acquire))
                          {
                              return;
                          }
                          if (const auto reader = state->reader.lock())
                          {
                              reader->on_tracks(tracks);
                          }
                      });
}

template <typename Frame>
void media_history<Frame>::dispatch_reader_end(const std::shared_ptr<media_reader_state_t<Frame>>& state)
{
    boost::asio::post(state->worker->io(),
                      [state]()
                      {
                          if (!state->active.load(std::memory_order_acquire))
                          {
                              return;
                          }
                          if (const auto reader = state->reader.lock())
                          {
                              reader->on_end();
                          }
                      });
}

}    // namespace media_server

#endif
