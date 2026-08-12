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
    std::vector<track_id> track_ids;
    std::atomic_bool active{true};
    std::atomic_bool read_outstanding{};
    std::atomic<media_reader_generation> generation{1};
    std::atomic<media_reader_generation> ready_generation{};
    std::uint64_t cursor{};
    bool cursor_initialized{};
    bool pending_read{};
    bool registered{};
    bool waiting_for_key_frame{};
};

namespace
{
constexpr std::size_t max_gop_cache_frames = 2500;

void release_read_outstanding(const std::shared_ptr<media_reader_state>& state, media_reader_generation generation)
{
    if (state->generation.load(std::memory_order_acquire) == generation)
    {
        state->read_outstanding.store(false, std::memory_order_release);
    }
}

bool reader_interested(const media_reader_state& state, track_id id)
{
    return state.track_ids.empty() || std::ranges::find(state.track_ids, id) != state.track_ids.end();
}

std::vector<media_track> reader_tracks(const media_reader_state& state, std::vector<media_track> tracks)
{
    if (state.track_ids.empty())
    {
        return tracks;
    }
    std::erase_if(tracks, [&state](const media_track& track) { return !reader_interested(state, track.id); });
    return tracks;
}
}

media_reader_handle::media_reader_handle(std::weak_ptr<media_stream> stream, std::shared_ptr<media_reader_state> state)
    : stream_(std::move(stream)), state_(std::move(state))
{
}

void media_reader_handle::async_read(media_reader_generation generation) const
{
    if (!state_ || !state_->active.load(std::memory_order_acquire) ||
        state_->generation.load(std::memory_order_acquire) != generation ||
        state_->ready_generation.load(std::memory_order_acquire) != generation)
    {
        return;
    }

    bool expected = false;
    if (!state_->read_outstanding.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
    {
        return;
    }
    if (!state_->active.load(std::memory_order_acquire) ||
        state_->generation.load(std::memory_order_acquire) != generation ||
        state_->ready_generation.load(std::memory_order_acquire) != generation)
    {
        state_->read_outstanding.store(false, std::memory_order_release);
        return;
    }
    if (const auto stream = stream_.lock())
    {
        stream->request_read(state_, generation);
        return;
    }
    state_->read_outstanding.store(false, std::memory_order_release);
}

void media_reader_handle::remove() const
{
    if (!state_ || !state_->active.exchange(false, std::memory_order_acq_rel))
    {
        return;
    }

    state_->ready_generation.store(0, std::memory_order_release);
    state_->generation.fetch_add(1, std::memory_order_acq_rel);
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

bool media_stream::ended() const noexcept { return ended_.load(std::memory_order_acquire); }

std::vector<media_track> media_stream::tracks() const
{
    const auto snapshot = track_snapshot_.load(std::memory_order_acquire);
    return snapshot ? *snapshot : std::vector<media_track>{};
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
    const std::weak_ptr<media_sink> weak_sink = sink;
    boost::asio::dispatch(owner_executor_,
                          [self, weak_sink]()
                      {
                          if (auto current = weak_sink.lock())
                          {
                              self->add_sink_on_owner(std::move(current));
                          }
                      });
}

void media_stream::remove_sink(const media_sink& sink)
{
    const auto self = weak_from_this().lock();
    if (!self)
    {
        return;
    }
    const auto* sink_address = &sink;
    boost::asio::dispatch(owner_executor_, [self, sink_address]() { self->remove_sink_on_owner(sink_address); });
}

media_reader_handle media_stream::add_reader(const std::shared_ptr<media_reader>& reader,
                                             boost::asio::any_io_executor executor,
                                             std::vector<track_id> track_ids)
{
    const auto self = weak_from_this().lock();
    if (!reader || !executor || !self)
    {
        return {};
    }

    auto state = std::make_shared<media_reader_state>();
    state->reader = reader;
    state->executor = std::move(executor);
    state->track_ids = std::move(track_ids);

    media_reader_handle handle(self, state);
    reader->handle_ = handle;

    boost::asio::dispatch(owner_executor_, [self, state]() { self->add_reader_on_owner(state); });
    return handle;
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
    if (tracks_.at(id).kind == media_kind::video)
    {
        current_gop_start_sequence_.reset();
        current_gop_frames_ = 0;
    }
    reset_readers(id, tracks_.at(id).kind);
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

    const auto sequence = next_reader_sequence_++;
    append_reader_history(sequence, frame, track->second);
    dispatch_pending_readers(sequence, frame);
    dispatch_frame(frame);
}

void media_stream::end()
{
    if (ended_.exchange(true, std::memory_order_acq_rel))
    {
        return;
    }

    gop_cache_.clear();
    reset_reader_history();
    end_readers();
    remove_inactive_sinks();

    std::vector<std::shared_ptr<media_sink>> sinks;
    sinks.reserve(sinks_.size());
    for (const auto& weak_sink : sinks_)
    {
        if (auto sink = weak_sink.lock())
        {
            sinks.push_back(std::move(sink));
        }
    }

    sinks_.clear();

    for (const auto& sink : sinks)
    {
        sink->on_end();
    }
}

void media_stream::add_sink_on_owner(std::shared_ptr<media_sink> sink)
{
    if (ended())
    {
        sink->on_end();
        return;
    }

    remove_inactive_sinks();
    if (has_sink(*sink))
    {
        return;
    }

    sinks_.push_back(sink);

    replay_sink(sink, tracks(), gop_cache_);
}

void media_stream::remove_sink_on_owner(const media_sink* sink)
{
    if (sink != nullptr)
    {
        std::erase_if(sinks_,
                      [sink](const std::weak_ptr<media_sink>& weak_sink)
                      {
                          const auto current = weak_sink.lock();
                          return !current || current.get() == sink;
                      });
    }
    remove_inactive_sinks();
}

void media_stream::remove_inactive_sinks()
{
    std::erase_if(sinks_, [](const std::weak_ptr<media_sink>& sink) { return sink.expired(); });
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
    for (const auto& weak_sink : sinks_)
    {
        const auto current = weak_sink.lock();
        if (current && current.get() == &sink)
        {
            return true;
        }
    }
    return false;
}

void media_stream::replay_sink(const std::shared_ptr<media_sink>& sink,
                               std::vector<media_track> tracks,
                               std::vector<media_frame> frames)
{
    for (const auto& track : tracks)
    {
        if (ended() || !has_sink(*sink))
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
        if (ended() || !has_sink(*sink))
        {
            return;
        }
        sink->on_frame(frame);
    }
}

void media_stream::dispatch_track(const media_track& track)
{
    const auto id = track.id;
    const auto config_version = track.config_version;
    const auto snapshot = sinks_;
    for (const auto& weak_sink : snapshot)
    {
        const auto current = tracks_.find(id);
        if (current == tracks_.end() || current->second.config_version != config_version)
        {
            return;
        }
        if (const auto sink = weak_sink.lock(); sink && has_sink(*sink))
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
    const auto snapshot = sinks_;
    for (const auto& weak_sink : snapshot)
    {
        if (const auto sink = weak_sink.lock(); sink && has_sink(*sink))
        {
            sink->on_frame(frame);
            if (ended())
            {
                return;
            }
        }
    }
}

void media_stream::request_read(const std::shared_ptr<media_reader_state>& state, media_reader_generation generation)
{
    if (!state || !state->active.load(std::memory_order_acquire) ||
        state->generation.load(std::memory_order_acquire) != generation ||
        state->ready_generation.load(std::memory_order_acquire) != generation)
    {
        if (state)
        {
            release_read_outstanding(state, generation);
        }
        return;
    }
    const auto self = weak_from_this().lock();
    if (!self)
    {
        release_read_outstanding(state, generation);
        return;
    }
    boost::asio::dispatch(owner_executor_, [self, state, generation]() { self->request_read_on_owner(state, generation); });
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
    if (ended())
    {
        dispatch_reader_end(state, state->generation.load(std::memory_order_acquire));
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
    dispatch_reader_reset(state, reader_tracks(*state, tracks()), state->generation.load(std::memory_order_acquire));
}

void media_stream::request_read_on_owner(const std::shared_ptr<media_reader_state>& state, media_reader_generation generation)
{
    if (ended() || !state->registered || state->pending_read || !state->active.load(std::memory_order_acquire) ||
        state->generation.load(std::memory_order_acquire) != generation ||
        state->ready_generation.load(std::memory_order_acquire) != generation)
    {
        release_read_outstanding(state, generation);
        return;
    }

    state->pending_read = true;
    complete_reader_from_history(state, generation);
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

void media_stream::reset_reader_history()
{
    reader_history_.clear();
    current_gop_start_sequence_.reset();
    current_gop_frames_ = 0;
}

void media_stream::reset_readers(track_id changed_track, media_kind kind)
{
    remove_inactive_readers();
    const auto snapshot = tracks();
    for (const auto& state : readers_)
    {
        if (!reader_interested(*state, changed_track))
        {
            continue;
        }
        state->ready_generation.store(0, std::memory_order_release);
        const auto generation = state->generation.fetch_add(1, std::memory_order_acq_rel) + 1;
        state->read_outstanding.store(false, std::memory_order_release);
        state->cursor = next_reader_sequence_;
        state->cursor_initialized = true;
        state->pending_read = false;
        state->waiting_for_key_frame = state->waiting_for_key_frame || kind == media_kind::video;
        dispatch_reader_reset(state, reader_tracks(*state, snapshot), generation);
    }
}

void media_stream::end_readers()
{
    remove_inactive_readers();
    auto readers = std::move(readers_);
    readers_.clear();
    for (const auto& state : readers)
    {
        state->ready_generation.store(0, std::memory_order_release);
        const auto generation = state->generation.fetch_add(1, std::memory_order_acq_rel) + 1;
        state->read_outstanding.store(false, std::memory_order_release);
        state->registered = false;
        state->cursor_initialized = false;
        state->pending_read = false;
        state->waiting_for_key_frame = false;
        dispatch_reader_end(state, generation);
    }
}

void media_stream::append_reader_history(std::uint64_t sequence, const media_frame& frame, const media_track& track)
{
    if (!has_video_track())
    {
        return;
    }

    if (track.kind == media_kind::video && frame.key_frame)
    {
        if (current_gop_start_sequence_)
        {
            const auto previous_start = *current_gop_start_sequence_;
            std::erase_if(reader_history_, [previous_start](const media_history_entry& entry) { return entry.sequence < previous_start; });
        }
        current_gop_start_sequence_ = sequence;
        current_gop_frames_ = 0;
    }

    if (!current_gop_start_sequence_ && reader_history_.empty())
    {
        return;
    }
    if (current_gop_frames_ >= max_gop_cache_frames)
    {
        reset_reader_history();
        return;
    }

    reader_history_.push_back(media_history_entry{.sequence = sequence, .config_version = track.config_version, .frame = frame});
    ++current_gop_frames_;
}

void media_stream::dispatch_pending_readers(std::uint64_t sequence, const media_frame& frame)
{
    remove_inactive_readers();
    const bool video_stream = has_video_track();
    for (const auto& state : readers_)
    {
        if (!state->pending_read || !state->active.load(std::memory_order_acquire))
        {
            continue;
        }
        const auto generation = state->generation.load(std::memory_order_acquire);
        if (state->ready_generation.load(std::memory_order_acquire) != generation)
        {
            continue;
        }

        if (!reader_interested(*state, frame.track))
        {
            if (state->cursor_initialized && state->cursor == sequence)
            {
                state->cursor = sequence + 1;
            }
            complete_reader_from_history(state, generation);
            continue;
        }

        if (state->waiting_for_key_frame)
        {
            const auto track = tracks_.find(frame.track);
            if (frame.key_frame && track != tracks_.end() && track->second.kind == media_kind::video)
            {
                state->waiting_for_key_frame = false;
                deliver_reader_frame(state, generation, sequence, frame);
            }
            continue;
        }

        if (!video_stream || (state->cursor_initialized && state->cursor == sequence))
        {
            deliver_reader_frame(state, generation, sequence, frame);
            continue;
        }
        complete_reader_from_history(state, generation);
    }
}

void media_stream::complete_reader_from_history(const std::shared_ptr<media_reader_state>& state,
                                                media_reader_generation generation)
{
    if (!state->pending_read || reader_history_.empty())
    {
        return;
    }

    const auto first_sequence = reader_history_.front().sequence;
    if (!state->cursor_initialized || state->cursor < first_sequence)
    {
        if (!current_gop_start_sequence_)
        {
            return;
        }
        state->cursor = *current_gop_start_sequence_;
        state->cursor_initialized = true;
    }
    if (state->cursor < first_sequence || state->cursor > reader_history_.back().sequence)
    {
        return;
    }

    auto iterator = std::ranges::find_if(reader_history_,
                                         [this, &state](const media_history_entry& entry)
                                         {
                                             const auto track = tracks_.find(entry.frame.track);
                                             return entry.sequence >= state->cursor && reader_interested(*state, entry.frame.track) &&
                                                    track != tracks_.end() && track->second.config_version == entry.config_version &&
                                                    (!state->waiting_for_key_frame ||
                                                     (track->second.kind == media_kind::video && entry.frame.key_frame));
                                         });
    if (iterator == reader_history_.end())
    {
        return;
    }

    if (state->waiting_for_key_frame)
    {
        state->waiting_for_key_frame = false;
    }
    const auto entry = *iterator;
    deliver_reader_frame(state, generation, entry.sequence, entry.frame);
}

void media_stream::deliver_reader_frame(const std::shared_ptr<media_reader_state>& state,
                                        media_reader_generation generation,
                                        std::uint64_t sequence,
                                        media_frame frame)
{
    if (!state->pending_read || !state->active.load(std::memory_order_acquire) ||
        state->generation.load(std::memory_order_acquire) != generation ||
        state->ready_generation.load(std::memory_order_acquire) != generation)
    {
        return;
    }

    state->pending_read = false;
    state->cursor = sequence + 1;
    state->cursor_initialized = true;
    boost::asio::post(state->executor,
                      [state, generation, frame = std::move(frame)]() mutable
                      {
                          if (!state->active.load(std::memory_order_acquire) ||
                              state->generation.load(std::memory_order_acquire) != generation ||
                              state->ready_generation.load(std::memory_order_acquire) != generation)
                          {
                              release_read_outstanding(state, generation);
                              return;
                          }
                          if (const auto reader = state->reader.lock())
                          {
                              state->read_outstanding.store(false, std::memory_order_release);
                              reader->on_read(generation, std::move(frame));
                              return;
                          }
                          release_read_outstanding(state, generation);
                      });
}

void media_stream::dispatch_reader_reset(const std::shared_ptr<media_reader_state>& state,
                                         std::vector<media_track> tracks,
                                         media_reader_generation generation)
{
    boost::asio::post(state->executor,
                      [state, tracks = std::move(tracks), generation]()
                      {
                          if (!state->active.load(std::memory_order_acquire) ||
                              state->generation.load(std::memory_order_acquire) != generation)
                          {
                              return;
                          }
                          const auto reader = state->reader.lock();
                          if (!reader)
                          {
                              return;
                          }
                          for (const auto& track : tracks)
                          {
                              if (!state->active.load(std::memory_order_acquire) ||
                                  state->generation.load(std::memory_order_acquire) != generation)
                              {
                                  return;
                              }
                              reader->on_track(generation, track);
                          }
                          if (!state->active.load(std::memory_order_acquire) ||
                              state->generation.load(std::memory_order_acquire) != generation)
                          {
                              return;
                          }
                          state->ready_generation.store(generation, std::memory_order_release);
                          if (!state->active.load(std::memory_order_acquire) ||
                              state->generation.load(std::memory_order_acquire) != generation)
                          {
                              return;
                          }
                          reader->on_ready(generation);
                      });
}

void media_stream::dispatch_reader_end(const std::shared_ptr<media_reader_state>& state, media_reader_generation generation)
{
    boost::asio::post(state->executor,
                      [state, generation]()
                      {
                          if (!state->active.load(std::memory_order_acquire) ||
                              state->generation.load(std::memory_order_acquire) != generation)
                          {
                              return;
                          }
                          if (const auto reader = state->reader.lock())
                          {
                              reader->on_end(generation);
                          }
                      });
}

bool media_stream::has_video_track() const
{
    return std::ranges::any_of(tracks_, [](const auto& value) { return value.second.kind == media_kind::video; });
}

}    // namespace media_server
