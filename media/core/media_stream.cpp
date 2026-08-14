#include "media/core/media_stream.h"

#include <boost/asio/dispatch.hpp>
#include <boost/asio/post.hpp>

#include <algorithm>
#include <utility>

namespace media_server
{
struct media_reader_state
{
    std::weak_ptr<media_reader> reader;
    boost::asio::any_io_executor executor;
    std::atomic_bool active{true};
    std::atomic_bool terminal{};
    std::atomic_bool read_outstanding{};
    media_reader_cursor pending_cursor;
    bool pending_read{};
    bool registered{};
};

namespace
{
constexpr std::size_t max_gop_frames = 2500;
constexpr std::size_t max_read_batch_entries = 128;

void release_read_outstanding(const std::shared_ptr<media_reader_state>& state)
{
    state->read_outstanding.store(false, std::memory_order_release);
}
}

media_reader_handle::media_reader_handle(std::weak_ptr<media_stream> stream, std::shared_ptr<media_reader_state> state)
    : stream_(std::move(stream)), state_(std::move(state))
{
}

void media_reader_handle::async_read(media_reader_cursor cursor) const
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
        release_read_outstanding(state_);
        return;
    }
    if (const auto stream = stream_.lock())
    {
        stream->request_read(state_, cursor);
        return;
    }
    release_read_outstanding(state_);
}

void media_reader_handle::remove() const
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

media_stream::media_stream(std::string name, boost::asio::any_io_executor owner_executor)
    : name_(std::move(name)), owner_executor_(std::move(owner_executor))
{
}

const std::string& media_stream::name() const noexcept { return name_; }

std::vector<media_track> media_stream::tracks() const
{
    const auto snapshot = track_snapshot_.load(std::memory_order_acquire);
    return snapshot ? snapshot->tracks : std::vector<media_track>{};
}

void media_stream::add_sink(const std::shared_ptr<media_sink>& sink)
{
    if (!sink)
    {
        return;
    }
    const auto self = weak_from_this().lock();
    if (!self)
    {
        return;
    }
    boost::asio::dispatch(owner_executor_, [self, sink]() { self->add_sink_on_owner(sink); });
}

media_reader_handle media_stream::add_reader(const std::shared_ptr<media_reader>& reader,
                                             boost::asio::any_io_executor executor)
{
    const auto self = weak_from_this().lock();
    if (!reader || !executor || !self)
    {
        return {};
    }

    auto state = std::make_shared<media_reader_state>();
    state->reader = reader;
    state->executor = std::move(executor);

    media_reader_handle handle(self, state);
    reader->handle_ = handle;

    boost::asio::dispatch(owner_executor_, [self, state]() { self->add_reader_on_owner(state); });
    return handle;
}

bool media_stream::set_tracks(std::vector<media_track> tracks)
{
    if (ended_ || !tracks_.empty() || tracks.empty())
    {
        return false;
    }

    std::map<track_id, media_track> initial_tracks;
    for (auto& track : tracks)
    {
        if (track.id == 0 || track.codec_config.empty())
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
    if (sink_)
    {
        for (const auto& [id, track] : tracks_)
        {
            static_cast<void>(id);
            sink_->on_track(track);
        }
    }
    return true;
}

bool media_stream::update_track(media_track track)
{
    if (ended_ || track.id == 0 || track.codec_config.empty())
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
    sink_replay_barrier_sequence_ = next_history_sequence_;
    tracks_.insert_or_assign(id, std::move(track));
    publish_track_snapshot();
    if (tracks_.at(id).kind == media_kind::video)
    {
        current_gop_start_sequence_.reset();
        current_gop_frames_ = 0;
    }
    dispatch_reader_tracks(track_snapshot_.load(std::memory_order_acquire));
    if (sink_)
    {
        sink_->on_track(tracks_.at(id));
    }
    return true;
}

void media_stream::publish(media_frame frame)
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
    if (sink_)
    {
        sink_->on_frame(frame);
    }
}

void media_stream::end()
{
    if (ended_)
    {
        return;
    }
    ended_ = true;

    reset_history();
    end_readers();

    auto sink = std::move(sink_);
    if (sink)
    {
        sink->on_end();
    }
}

void media_stream::add_sink_on_owner(std::shared_ptr<media_sink> sink)
{
    if (ended_)
    {
        sink->on_end();
        return;
    }

    if (sink_)
    {
        return;
    }

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

    sink_ = std::move(sink);
    for (const auto& [id, track] : tracks_)
    {
        static_cast<void>(id);
        sink_->on_track(track);
    }
    for (const auto& frame : frames)
    {
        sink_->on_frame(frame);
    }
}

void media_stream::publish_track_snapshot()
{
    auto snapshot = std::make_shared<media_track_snapshot>();
    snapshot->revision = ++track_revision_;
    snapshot->tracks.reserve(tracks_.size());
    for (const auto& [id, track] : tracks_)
    {
        static_cast<void>(id);
        snapshot->tracks.push_back(track);
    }
    track_snapshot_.store(std::move(snapshot), std::memory_order_release);
}

void media_stream::request_read(const std::shared_ptr<media_reader_state>& state, media_reader_cursor cursor)
{
    if (!state || !state->active.load(std::memory_order_acquire) || state->terminal.load(std::memory_order_acquire))
    {
        if (state)
        {
            release_read_outstanding(state);
        }
        return;
    }
    const auto self = weak_from_this().lock();
    if (!self)
    {
        release_read_outstanding(state);
        return;
    }
    boost::asio::dispatch(owner_executor_, [self, state, cursor]() { self->request_read_on_owner(state, cursor); });
}

void media_stream::remove_reader(const std::shared_ptr<media_reader_state>& state)
{
    if (!state)
    {
        return;
    }
    const auto self = weak_from_this().lock();
    if (!self)
    {
        return;
    }
    boost::asio::dispatch(owner_executor_, [self, state]() { self->remove_reader_on_owner(state); });
}

void media_stream::add_reader_on_owner(const std::shared_ptr<media_reader_state>& state)
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

void media_stream::request_read_on_owner(const std::shared_ptr<media_reader_state>& state, media_reader_cursor cursor)
{
    if (ended_ || !state->registered || state->pending_read || !state->active.load(std::memory_order_acquire) ||
        state->terminal.load(std::memory_order_acquire))
    {
        release_read_outstanding(state);
        return;
    }

    state->pending_read = true;
    state->pending_cursor = cursor;
    complete_reader_from_history(state);
}

void media_stream::remove_reader_on_owner(const std::shared_ptr<media_reader_state>& state)
{
    state->registered = false;
    state->pending_read = false;
    state->read_outstanding.store(false, std::memory_order_release);
    std::erase(readers_, state);
}

void media_stream::remove_inactive_readers()
{
    std::erase_if(readers_,
                  [](const std::shared_ptr<media_reader_state>& state)
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

void media_stream::reset_history()
{
    history_.clear();
    current_gop_start_sequence_.reset();
    current_gop_frames_ = 0;
}

void media_stream::dispatch_reader_tracks(const media_track_snapshot_ptr& tracks)
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

void media_stream::end_readers()
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

void media_stream::append_history(std::uint64_t sequence, const media_frame& frame, const media_track& track)
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
    if (current_gop_frames_ >= max_gop_frames)
    {
        reset_history();
        return;
    }

    history_.push_back(media_history_entry{.sequence = sequence, .config_version = track.config_version, .frame = frame});
    ++current_gop_frames_;
}

void media_stream::dispatch_pending_readers()
{
    remove_inactive_readers();
    for (const auto& state : readers_)
    {
        if (state->pending_read && state->active.load(std::memory_order_acquire) && !state->terminal.load(std::memory_order_acquire))
        {
            complete_reader_from_history(state);
        }
    }
}

void media_stream::complete_reader_from_history(const std::shared_ptr<media_reader_state>& state)
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

    media_read_batch batch;
    batch.tracks = track_snapshot_.load(std::memory_order_acquire);
    batch.entries.reserve(max_read_batch_entries);
    for (; iterator != history_.end() && batch.entries.size() < max_read_batch_entries; ++iterator)
    {
        batch.next_cursor = iterator->sequence + 1;
        batch.entries.push_back(media_read_entry{.config_version = iterator->config_version, .frame = iterator->frame});
    }

    if (batch.entries.empty())
    {
        return;
    }

    state->pending_read = false;
    deliver_reader_batch(state, std::move(batch));
}

void media_stream::deliver_reader_batch(const std::shared_ptr<media_reader_state>& state, media_read_batch batch)
{
    if (!state->active.load(std::memory_order_acquire) || state->terminal.load(std::memory_order_acquire))
    {
        release_read_outstanding(state);
        return;
    }

    boost::asio::post(state->executor,
                      [state, batch = std::move(batch)]() mutable
                      {
                          if (!state->active.load(std::memory_order_acquire) || state->terminal.load(std::memory_order_acquire))
                          {
                              release_read_outstanding(state);
                              return;
                          }
                          if (const auto reader = state->reader.lock())
                          {
                              state->read_outstanding.store(false, std::memory_order_release);
                              reader->on_read(std::move(batch));
                              return;
                          }
                          release_read_outstanding(state);
                      });
}

void media_stream::dispatch_reader_tracks(const std::shared_ptr<media_reader_state>& state, media_track_snapshot_ptr tracks)
{
    if (!tracks)
    {
        return;
    }

    boost::asio::post(state->executor,
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

void media_stream::dispatch_reader_end(const std::shared_ptr<media_reader_state>& state)
{
    boost::asio::post(state->executor,
                      [state]()
                      {
                          if (!state->active.load(std::memory_order_acquire) || !state->terminal.load(std::memory_order_acquire))
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
