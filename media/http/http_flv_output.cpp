#include "media/http/http_flv_output.h"

#include <utility>

namespace media_server
{
http_flv_output::http_flv_output(write_handler on_write, end_handler on_end)
    : on_write_(std::move(on_write)),
      on_end_(std::move(on_end)),
      muxer_(
          [this](int type, std::span<const std::uint8_t> data, std::uint32_t timestamp)
          {
              static_cast<void>(timestamp);
              if (writer_ != nullptr)
              {
                  static_cast<void>(flv_writer_input(writer_, type, data.data(), data.size(), timestamp));
              }
          })
{
}

http_flv_output::~http_flv_output()
{
    if (writer_ != nullptr)
    {
        flv_writer_destroy(writer_);
    }
}

void http_flv_output::on_tracks(media_track_snapshot_ptr tracks)
{
    if (ended_)
    {
        return;
    }
    static_cast<void>(apply_tracks(tracks));
}

void http_flv_output::on_read(media_read_batch batch)
{
    if (ended_)
    {
        return;
    }

    reader_cursor_ = batch.next_cursor;
    batch_ = std::move(batch);
    batch_index_ = 0;
    batch_active_ = true;
    if (apply_tracks(batch_.tracks))
    {
        return;
    }
    process_batch();
}

void http_flv_output::on_end() { finish(); }

void http_flv_output::write_complete(std::uint64_t generation)
{
    if (ended_ || generation_ != generation)
    {
        return;
    }
    process_batch();
}

bool http_flv_output::apply_tracks(const media_track_snapshot_ptr& tracks)
{
    if (!tracks || tracks->revision <= track_revision_)
    {
        return false;
    }

    bool has_audio = false;
    bool has_video = false;
    bool video_changed = false;
    for (const auto& track : tracks->tracks)
    {
        has_audio = has_audio || track.kind == media_kind::audio;
        has_video = has_video || track.kind == media_kind::video;
        const auto current = reader_tracks_.find(track.id);
        video_changed = video_changed ||
                        (current != reader_tracks_.end() && track.kind == media_kind::video &&
                         current->second.config_version != track.config_version);
    }

    reader_tracks_.clear();
    output_buffer_.clear();
    if (writer_ == nullptr)
    {
        writer_ = flv_writer_create2(has_audio ? 1 : 0, has_video ? 1 : 0, &http_flv_output::writer_callback, this);
    }
    for (const auto& track : tracks->tracks)
    {
        reader_tracks_.emplace(track.id, track);
        muxer_.on_track(track);
    }

    track_revision_ = tracks->revision;
    waiting_for_key_frame_ = waiting_for_key_frame_ || video_changed;
    ++generation_;
    if (on_write_)
    {
        on_write_(generation_, std::move(output_buffer_), true);
    }
    return true;
}

void http_flv_output::process_batch()
{
    if (ended_)
    {
        return;
    }
    if (!batch_active_)
    {
        reader_handle().async_read(reader_cursor_);
        return;
    }

    while (batch_index_ < batch_.entries.size())
    {
        auto& entry = batch_.entries[batch_index_++];
        const auto track = reader_tracks_.find(entry.frame.track);
        if (track == reader_tracks_.end() || track->second.config_version != entry.config_version)
        {
            continue;
        }
        if (waiting_for_key_frame_)
        {
            if (track->second.kind != media_kind::video || !entry.frame.key_frame)
            {
                continue;
            }
            waiting_for_key_frame_ = false;
        }

        output_buffer_.clear();
        muxer_.on_frame(entry.frame);
        if (output_buffer_.empty())
        {
            continue;
        }
        if (on_write_)
        {
            on_write_(generation_, std::move(output_buffer_), false);
        }
        return;
    }

    batch_ = {};
    batch_index_ = 0;
    batch_active_ = false;
    reader_handle().async_read(reader_cursor_);
}

void http_flv_output::finish()
{
    if (ended_)
    {
        return;
    }
    ended_ = true;
    if (on_end_)
    {
        on_end_();
    }
}

int http_flv_output::writer_callback(void* param, const flv_vec_t* vectors, int count)
{
    auto* self = static_cast<http_flv_output*>(param);
    if (!self->on_write_ || vectors == nullptr || count <= 0)
    {
        return 0;
    }

    std::size_t bytes = 0;
    for (int index = 0; index < count; ++index)
    {
        if (vectors[index].ptr != nullptr && vectors[index].len > 0)
        {
            bytes += static_cast<std::size_t>(vectors[index].len);
        }
    }

    self->output_buffer_.reserve(self->output_buffer_.size() + bytes);
    for (int index = 0; index < count; ++index)
    {
        if (vectors[index].ptr == nullptr || vectors[index].len <= 0)
        {
            continue;
        }
        const auto* begin = static_cast<const std::uint8_t*>(vectors[index].ptr);
        self->output_buffer_.insert(self->output_buffer_.end(), begin, begin + vectors[index].len);
    }
    return 0;
}
}    // namespace media_server
