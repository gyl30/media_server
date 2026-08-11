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

void http_flv_output::on_track(media_reader_generation generation, const media_track& track)
{
    if (ended_)
    {
        return;
    }
    if (generation_ != generation)
    {
        generation_ = generation;
        pending_tracks_.clear();
    }
    pending_tracks_.push_back(track);
}

void http_flv_output::on_ready(media_reader_generation generation)
{
    if (ended_ || generation_ != generation)
    {
        return;
    }

    std::vector<track_id> track_ids;
    bool has_audio = false;
    bool has_video = false;
    track_ids.reserve(pending_tracks_.size());
    for (const auto& track : pending_tracks_)
    {
        track_ids.push_back(track.id);
        has_audio = has_audio || track.kind == media_kind::audio;
        has_video = has_video || track.kind == media_kind::video;
    }

    if (writer_ != nullptr && track_ids != track_ids_)
    {
        finish();
        return;
    }

    output_buffer_.clear();
    if (writer_ == nullptr)
    {
        track_ids_ = std::move(track_ids);
        writer_ = flv_writer_create2(has_audio ? 1 : 0, has_video ? 1 : 0, &http_flv_output::writer_callback, this);
    }
    for (const auto& track : pending_tracks_)
    {
        muxer_.on_track(track);
    }
    pending_tracks_.clear();

    if (on_write_)
    {
        on_write_(generation, std::move(output_buffer_), true);
    }
}

void http_flv_output::on_read(media_reader_generation generation, media_frame frame)
{
    if (ended_ || generation_ != generation)
    {
        return;
    }

    output_buffer_.clear();
    muxer_.on_frame(frame);
    if (output_buffer_.empty())
    {
        reader_handle().async_read(generation);
        return;
    }
    if (on_write_)
    {
        on_write_(generation, std::move(output_buffer_), false);
    }
}

void http_flv_output::on_end(media_reader_generation generation)
{
    static_cast<void>(generation);
    finish();
}

void http_flv_output::write_complete(media_reader_generation generation)
{
    if (!ended_ && generation_ == generation)
    {
        reader_handle().async_read(generation);
    }
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
