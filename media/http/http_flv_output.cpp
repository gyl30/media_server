#include "media/http/http_flv_output.h"

#include <utility>

namespace media_server
{
http_flv_output::http_flv_output(std::span<const media_track> tracks, write_handler on_write, end_handler on_end)
    : on_write_(std::move(on_write)), on_end_(std::move(on_end)), muxer_([this](int type, std::span<const std::uint8_t> data, std::uint32_t timestamp) {
          static_cast<void>(timestamp);
          if (writer_ != nullptr)
          {
              static_cast<void>(flv_writer_input(writer_, type, data.data(), data.size(), timestamp));
          }
      })
{
    bool has_audio = false;
    bool has_video = false;
    for (const auto& track : tracks)
    {
        has_audio = has_audio || track.kind == media_kind::audio;
        has_video = has_video || track.kind == media_kind::video;
    }

    writer_ = flv_writer_create2(has_audio ? 1 : 0, has_video ? 1 : 0, &http_flv_output::writer_callback, this);
}

http_flv_output::~http_flv_output()
{
    if (writer_ != nullptr)
    {
        flv_writer_destroy(writer_);
    }
}

void http_flv_output::on_track(const media_track& track)
{
    muxer_.on_track(track);
}

void http_flv_output::on_frame(const media_frame& frame)
{
    muxer_.on_frame(frame);
}

void http_flv_output::on_end()
{
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

    std::vector<std::uint8_t> data;
    data.reserve(bytes);
    for (int index = 0; index < count; ++index)
    {
        if (vectors[index].ptr == nullptr || vectors[index].len <= 0)
        {
            continue;
        }
        const auto* begin = static_cast<const std::uint8_t*>(vectors[index].ptr);
        data.insert(data.end(), begin, begin + vectors[index].len);
    }

    if (!data.empty())
    {
        self->on_write_(data);
    }
    return 0;
}
}    // namespace media_server
